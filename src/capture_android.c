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
#include "bsdr/webcam.h"
#include "bsdr_android.h"
#include <string.h>

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

/* keyframe (IDR) request, published to the JNI bridge: a new consumer joining bumps the generation,
 * and Kotlin asks MediaCodec for a sync frame (PARAMETER_KEY_REQUEST_SYNC_FRAME) on the next edge. */
static int g_keyframe_gen, g_keyframe_seen;

/* 2D->3D config, published to the JNI bridge (the Kotlin GL pipeline applies SBS on the
 * MediaProjection frame before MediaCodec, since C never sees a raw frame here). */
static int g_td_mode, g_td_deep, g_td_conv, g_td_swap, g_td_full, g_td_tier;
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

/* Face swap on Android runs in the Kotlin GL path (not this C capture / the desktop plugin). Image
 * decode needs ffmpeg (absent on Android) -> unavailable. */
int bsdr_capture_decode_image_rgb(const char *p, uint8_t **rgb, int *w, int *h) {
    (void)p; (void)rgb; (void)w; (void)h; return -1;
}

/* 2D->3D isn't applied in this C capture on Android (frames are HW-encoded; SBS runs in the Kotlin
 * MediaProjection -> GL pipeline). The engine moved to the 2d-3d plugin; the JNI drives depth directly
 * (see bsdr_jni.c). No capture-side threed hook is needed here. */

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

void bsdr_android_publish_threed(int mode, int deepness, int convergence, int swap, int full, int tier) {
    if (!g_lock) bsdr_android_capture_init();
    bsdr_mutex_lock(g_lock);
    if (mode != g_td_mode || deepness != g_td_deep || convergence != g_td_conv ||
        swap != g_td_swap || full != g_td_full || tier != g_td_tier) {
        g_td_mode = mode; g_td_deep = deepness; g_td_conv = convergence; g_td_swap = swap;
        g_td_full = full; g_td_tier = tier;
        g_td_gen++;
    }
    bsdr_mutex_unlock(g_lock);
}

int bsdr_android_poll_threed(int *mode, int *deepness, int *convergence, int *swap, int *full, int *tier) {
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
        if (tier)        *tier        = g_td_tier;
        changed = 1;
    }
    bsdr_mutex_unlock(g_lock);
    return changed;
}

/* ---- webcam source (published to the Kotlin capture switch) + camera list (from Kotlin) ---------
 * On Android the raw frame lives in the Kotlin capture path, so the C side just relays the operator's
 * source choice (desktop/webcam/webcam3d + device ids) to Kotlin via a polled generation counter,
 * and caches the CameraManager-enumerated list for the web-UI /api/webcams (bsdr_webcam_list). */
static char g_src_mode[16], g_src_dev[256], g_src_dev2[256];
static int  g_src_gen, g_src_seen;

void bsdr_android_publish_source(const char *mode, const char *dev, const char *dev2) {
    if (!g_lock) bsdr_android_capture_init();
    char m[16] = "", d[256] = "", d2[256] = "";
    snprintf(m, sizeof m, "%s", mode ? mode : "");
    snprintf(d, sizeof d, "%s", dev ? dev : "");
    snprintf(d2, sizeof d2, "%s", dev2 ? dev2 : "");
    bsdr_mutex_lock(g_lock);
    if (strcmp(m, g_src_mode) || strcmp(d, g_src_dev) || strcmp(d2, g_src_dev2)) {
        snprintf(g_src_mode, sizeof g_src_mode, "%s", m);
        snprintf(g_src_dev,  sizeof g_src_dev,  "%s", d);
        snprintf(g_src_dev2, sizeof g_src_dev2, "%s", d2);
        g_src_gen++;
    }
    bsdr_mutex_unlock(g_lock);
}

int bsdr_android_poll_source(char *mode, size_t ml, char *dev, size_t dl, char *dev2, size_t d2l) {
    if (!g_lock) return 0;
    int changed = 0;
    bsdr_mutex_lock(g_lock);
    if (g_src_gen != g_src_seen) {
        g_src_seen = g_src_gen;
        if (mode) snprintf(mode, ml, "%s", g_src_mode);
        if (dev)  snprintf(dev,  dl, "%s", g_src_dev);
        if (dev2) snprintf(dev2, d2l, "%s", g_src_dev2);
        changed = 1;
    }
    bsdr_mutex_unlock(g_lock);
    return changed;
}

#define BSDR_CAM_MAX 8
static bsdr_webcam_dev g_cams[BSDR_CAM_MAX];
static int g_cam_n;

void bsdr_android_set_cameras(const bsdr_webcam_dev *cams, int n) {
    if (!g_lock) bsdr_android_capture_init();
    if (n > BSDR_CAM_MAX) n = BSDR_CAM_MAX;
    bsdr_mutex_lock(g_lock);
    for (int i = 0; i < n; i++) g_cams[i] = cams[i];
    g_cam_n = n < 0 ? 0 : n;
    bsdr_mutex_unlock(g_lock);
}

/* ---- face swap config (published to the Kotlin GL faceswap stage) ---- */
static int  g_fs_on, g_fs_tier;
static char g_fs_src[512];
static int  g_fs_gen, g_fs_seen;

void bsdr_android_publish_faceswap(int on, int tier, const char *source) {
    if (!g_lock) bsdr_android_capture_init();
    char s[512] = ""; snprintf(s, sizeof s, "%s", source ? source : "");
    bsdr_mutex_lock(g_lock);
    if (on != g_fs_on || tier != g_fs_tier || strcmp(s, g_fs_src)) {
        g_fs_on = on; g_fs_tier = tier; snprintf(g_fs_src, sizeof g_fs_src, "%s", s);
        g_fs_gen++;
    }
    bsdr_mutex_unlock(g_lock);
}

int bsdr_android_poll_faceswap(int *on, int *tier, char *source, size_t sl) {
    if (!g_lock) return 0;
    int changed = 0;
    bsdr_mutex_lock(g_lock);
    if (g_fs_gen != g_fs_seen) {
        g_fs_seen = g_fs_gen;
        if (on) *on = g_fs_on;
        if (tier) *tier = g_fs_tier;
        if (source) snprintf(source, sl, "%s", g_fs_src);
        changed = 1;
    }
    bsdr_mutex_unlock(g_lock);
    return changed;
}

/* Consumed by webcam.c's Android branch (bsdr_webcam_list). */
int bsdr_android_cameras(bsdr_webcam_dev *out, int max) {
    if (!g_lock || !out) return 0;
    bsdr_mutex_lock(g_lock);
    int n = g_cam_n < max ? g_cam_n : max;
    for (int i = 0; i < n; i++) out[i] = g_cams[i];
    bsdr_mutex_unlock(g_lock);
    return n;
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

/* Request a keyframe on the next encoded frame. On Android the encoder is Kotlin's MediaCodec, so we
 * just publish an edge the JNI bridge polls (bsdr_android_poll_keyframe); MediaCodec's periodic IDR
 * covers consumers until Kotlin wires the explicit sync-frame request. */
void bsdr_capture_force_keyframe(bsdr_capture *c) {
    (void)c;
    if (!g_lock) return;
    bsdr_mutex_lock(g_lock);
    g_keyframe_gen++;
    bsdr_mutex_unlock(g_lock);
}

/* JNI bridge: returns 1 once after each force_keyframe() so Kotlin can ask MediaCodec for a sync
 * frame (edge-triggered, same pattern as bsdr_android_capture_want). */
int bsdr_android_poll_keyframe(void) {
    if (!g_lock) return 0;
    int changed = 0;
    bsdr_mutex_lock(g_lock);
    if (g_keyframe_gen != g_keyframe_seen) { g_keyframe_seen = g_keyframe_gen; changed = 1; }
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
