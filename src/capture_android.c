/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
/* Android capture: a thread-safe access-unit handoff, NOT an encoder. The Kotlin
 * side runs MediaProjection -> a MediaCodec AVC encoder (Constrained Baseline,
 * to match the Quest's OpenH264 receiver) and pushes the encoded Annex-B access
 * units in via the JNI bridge (bsdr_android_push_video). This shim implements
 * include/bsdr/capture.h as a small ring buffer the agent's video sender drains.
 *
 * SPS/PPS: MediaCodec emits the codec-config separately (BUFFER_FLAG_CODEC_CONFIG);
 * we cache it and prepend it to each IDR so SPS/PPS go in-band per keyframe (the
 * Quest caches SPS from pairing but the LAN/cloud senders expect it per IDR). */
#include "bsdr/capture.h"
#include "bsdr/threed.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr_android.h"

#include <stdlib.h>
#include <string.h>

#define AU_SLOTS 16

typedef struct { uint8_t *buf; size_t len; uint32_t rtp_ts; } au_slot;

struct bsdr_capture {
    int out_w, out_h, fps, bitrate;
    au_slot q[AU_SLOTS];
    int head, tail, count;
    bsdr_mutex *lock;
    uint8_t *cur;           /* last popped AU; valid until the next frame() call */
    size_t cur_cap;
};

/* The single active Android capture; MediaCodec pushes into it via JNI. The
 * agent worker opens/closes exactly one at a time. */
static bsdr_capture *g_cap;
static bsdr_mutex   *g_lock;        /* guards g_cap + cfg + want state */

/* cached codec-config (SPS/PPS) to prepend per IDR */
static uint8_t *g_cfg;
static size_t   g_cfg_len;

/* headset-requested quality, published to the JNI bridge */
static int g_want_w, g_want_h, g_want_fps, g_want_br;
static int g_want_gen, g_want_seen;

/* 2D->3D config, published to the JNI bridge (the Kotlin GL pipeline applies SBS on the
 * MediaProjection frame before MediaCodec, since C never sees a raw frame here). */
static int g_td_mode, g_td_deep, g_td_conv, g_td_swap, g_td_full;
static int g_td_gen, g_td_seen;

void bsdr_android_capture_init(void) {
    if (!g_lock) g_lock = bsdr_mutex_new();
}

/* Annex-B scan: is there an IDR (NAL type 5) slice in this access unit? */
static int au_has_idr(const uint8_t *p, size_t len) {
    for (size_t i = 0; i + 4 < len; i++) {
        if (p[i] == 0 && p[i+1] == 0 &&
            ((p[i+2] == 1 && (p[i+3] & 0x1f) == 5) ||
             (p[i+2] == 0 && p[i+3] == 1 && i + 4 < len && (p[i+4] & 0x1f) == 5)))
            return 1;
    }
    return 0;
}

bsdr_capture *bsdr_capture_open(const bsdr_capture_config *cfg) {
    bsdr_android_capture_init();
    bsdr_capture *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->out_w   = cfg->out_width  > 0 ? cfg->out_width  : (cfg->width  > 0 ? cfg->width  : 1080);
    c->out_h   = cfg->out_height > 0 ? cfg->out_height : (cfg->height > 0 ? cfg->height : 1920);
    c->fps     = cfg->fps     > 0 ? cfg->fps     : 30;
    c->bitrate = cfg->bitrate > 0 ? cfg->bitrate : 8000000;
    c->lock    = bsdr_mutex_new();

    bsdr_mutex_lock(g_lock);
    g_cap = c;
    /* publish the requested params for the JNI bridge to (re)configure MediaCodec */
    g_want_w = c->out_w; g_want_h = c->out_h; g_want_fps = c->fps; g_want_br = c->bitrate;
    g_want_gen++;
    bsdr_mutex_unlock(g_lock);

    BSDR_INFO("bsdr.capture", "android capture open: %dx%d @ %d fps, %d bps (MediaCodec)",
              c->out_w, c->out_h, c->fps, c->bitrate);
    return c;
}

void bsdr_capture_set_overlay(bsdr_capture *c, struct bsdr_overlay *ov) {
    (void)c; (void)ov;   /* overlay deferred on Android (web UI is the control surface) */
}

/* 2D->3D can't be applied here: Android capture hands us frames that MediaCodec already ENCODED on
 * the GPU (no raw NV12 in C). The SBS transform would have to run in the Kotlin MediaProjection ->
 * GL -> encoder-input-surface path. We accept and free the object so the shared agent links and the
 * web UI's 3D controls stay in sync; the transform is simply not applied on this build. */
void bsdr_capture_set_threed(bsdr_capture *c, struct bsdr_threed *t) {
    (void)c;
    if (t) {
        static int warned = 0;
        if (!warned) { warned = 1;
            BSDR_WARN("bsdr.capture", "2D->3D not applied on Android (HW-encoded frames); "
                      "SBS must be done in the MediaCodec/GL pipeline"); }
        bsdr_threed_close(t);
    }
}

void bsdr_android_push_video(const uint8_t *au, size_t len, int64_t pts_us, int is_config) {
    if (!au || !len || !g_lock) return;
    bsdr_mutex_lock(g_lock);
    bsdr_capture *c = g_cap;
    if (is_config) {                                  /* cache SPS/PPS, don't enqueue */
        uint8_t *n = realloc(g_cfg, len);
        if (n) { g_cfg = n; memcpy(g_cfg, au, len); g_cfg_len = len; }
        bsdr_mutex_unlock(g_lock);
        return;
    }
    if (!c) { bsdr_mutex_unlock(g_lock); return; }

    int idr = au_has_idr(au, len);
    size_t prefix = (idr && g_cfg_len) ? g_cfg_len : 0;
    size_t total  = prefix + len;
    uint8_t *buf  = malloc(total);
    if (!buf) { bsdr_mutex_unlock(g_lock); return; }
    if (prefix) memcpy(buf, g_cfg, prefix);           /* in-band SPS/PPS per IDR */
    memcpy(buf + prefix, au, len);
    bsdr_mutex_unlock(g_lock);

    bsdr_mutex_lock(c->lock);
    if (c->count == AU_SLOTS) {                        /* overflow: drop oldest */
        free(c->q[c->head].buf);
        c->head = (c->head + 1) % AU_SLOTS; c->count--;
    }
    c->q[c->tail].buf = buf;
    c->q[c->tail].len = total;
    c->q[c->tail].rtp_ts = (uint32_t)((pts_us < 0 ? 0 : pts_us) * 9 / 100);  /* 90 kHz */
    c->tail = (c->tail + 1) % AU_SLOTS; c->count++;
    bsdr_mutex_unlock(c->lock);
}

void bsdr_android_publish_threed(int mode, int deepness, int convergence, int swap, int full) {
    if (!g_lock) bsdr_android_capture_init();
    bsdr_mutex_lock(g_lock);
    if (mode != g_td_mode || deepness != g_td_deep || convergence != g_td_conv ||
        swap != g_td_swap || full != g_td_full) {
        g_td_mode = mode; g_td_deep = deepness; g_td_conv = convergence; g_td_swap = swap; g_td_full = full;
        g_td_gen++;
    }
    bsdr_mutex_unlock(g_lock);
}

int bsdr_android_poll_threed(int *mode, int *deepness, int *convergence, int *swap, int *full) {
    if (!g_lock) return 0;
    int changed = 0;
    bsdr_mutex_lock(g_lock);
    if (g_td_gen != g_td_seen) {
        g_td_seen = g_td_gen;
        if (mode)        *mode        = g_td_mode;
        if (deepness)    *deepness    = g_td_deep;
        if (convergence) *convergence = g_td_conv;
        if (swap)        *swap        = g_td_swap;
        if (full)        *full        = g_td_full;
        changed = 1;
    }
    bsdr_mutex_unlock(g_lock);
    return changed;
}

int bsdr_android_capture_want(int *width, int *height, int *fps, int *bitrate) {
    if (!g_lock) return 0;
    int changed = 0;
    bsdr_mutex_lock(g_lock);
    if (g_want_gen != g_want_seen) {
        g_want_seen = g_want_gen;
        if (width)   *width   = g_want_w;
        if (height)  *height  = g_want_h;
        if (fps)     *fps     = g_want_fps;
        if (bitrate) *bitrate = g_want_br;
        changed = 1;
    }
    bsdr_mutex_unlock(g_lock);
    return changed;
}

int bsdr_capture_frame(bsdr_capture *c, const uint8_t **au, size_t *len,
                       uint32_t *rtp_ts) {
    if (!c) return -1;
    unsigned interval = (unsigned)(1000 / (c->fps > 0 ? c->fps : 30));
    if (interval < 1) interval = 1;
    for (unsigned waited = 0; ; ) {
        bsdr_mutex_lock(c->lock);
        if (c->count > 0) {
            au_slot s = c->q[c->head];
            c->head = (c->head + 1) % AU_SLOTS; c->count--;
            bsdr_mutex_unlock(c->lock);
            free(c->cur);                              /* release the previous AU */
            c->cur = s.buf; c->cur_cap = s.len;
            *au = c->cur; *len = s.len; *rtp_ts = s.rtp_ts;
            return 1;
        }
        bsdr_mutex_unlock(c->lock);
        if (waited >= interval) return 0;              /* none this interval */
        bsdr_sleep_ms(2); waited += 2;
    }
}

void bsdr_capture_info(bsdr_capture *c, int *w, int *h, const char **enc) {
    if (w) *w = c->out_w;
    if (h) *h = c->out_h;
    if (enc) *enc = "mediacodec";
}

/* File media-bar controls: no-ops on Android (file playback is unsupported here — the agent casts
 * the live screen). Stubbed so agent.c / the media-bar code links. */
void bsdr_capture_seek(bsdr_capture *c, double frac) { (void)c; (void)frac; }
void bsdr_capture_set_paused(bsdr_capture *c, int paused) { (void)c; (void)paused; }
int  bsdr_capture_is_paused(bsdr_capture *c) { (void)c; return 0; }
double bsdr_capture_position(bsdr_capture *c, int *seekable) { (void)c; if (seekable) *seekable = 0; return 0.0; }

void bsdr_capture_close(bsdr_capture *c) {
    if (!c) return;
    bsdr_mutex_lock(g_lock);
    if (g_cap == c) g_cap = NULL;
    bsdr_mutex_unlock(g_lock);
    bsdr_mutex_lock(c->lock);
    while (c->count > 0) { free(c->q[c->head].buf); c->head = (c->head + 1) % AU_SLOTS; c->count--; }
    bsdr_mutex_unlock(c->lock);
    free(c->cur);
    bsdr_mutex_free(c->lock);
    free(c);
}
