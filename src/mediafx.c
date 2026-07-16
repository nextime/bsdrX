/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* Media-effect registry — see include/bsdr/mediafx.h. */
#include "bsdr/mediafx.h"
#include "bsdr/platform.h"

static bsdr_audio_fx_fn g_audio_fn = NULL;
static void            *g_audio_user = NULL;
static bsdr_mutex      *g_lock = NULL;   /* lazily created on first set (plugin load, single-threaded) */

static void mfx_lock(void)   { if (g_lock) bsdr_mutex_lock(g_lock); }
static void mfx_unlock(void) { if (g_lock) bsdr_mutex_unlock(g_lock); }

void bsdr_mediafx_set_audio(bsdr_audio_fx_fn fn, void *user) {
    if (!g_lock) g_lock = bsdr_mutex_new();
    mfx_lock();
    g_audio_fn = fn;
    g_audio_user = user;
    mfx_unlock();
}

int bsdr_mediafx_apply_audio(int16_t *pcm, int frames, int rate, int channels) {
    if (!g_audio_fn || !pcm || frames <= 0) return 0;   /* cheap fast-path when nothing is registered */
    /* Hold the lock across the call: a plugin unload (set NULL) then waits for the in-flight frame to
     * finish, so the effect callback is never invoked into unmapped plugin code. */
    mfx_lock();
    int handled = 0;
    if (g_audio_fn) { g_audio_fn(g_audio_user, pcm, frames, rate, channels); handled = 1; }
    mfx_unlock();
    return handled;
}

/* ---- same-dimensions video-effect chain ------------------------------------------------------- */
#define VFX_MAX 8
typedef struct { bsdr_video_fx_fn fn; void *user; int order; } vfx_slot;
static vfx_slot g_vfx[VFX_MAX];
static int      g_vfx_n = 0;

void bsdr_mediafx_video_add(bsdr_video_fx_fn fn, void *user, int order) {
    if (!g_lock) g_lock = bsdr_mutex_new();
    mfx_lock();
    /* remove any existing entry for this owner first (replace / fn==NULL => just remove) */
    for (int i = 0; i < g_vfx_n; i++)
        if (g_vfx[i].user == user) { g_vfx[i] = g_vfx[--g_vfx_n]; break; }
    if (fn && g_vfx_n < VFX_MAX) { g_vfx[g_vfx_n].fn = fn; g_vfx[g_vfx_n].user = user; g_vfx[g_vfx_n].order = order; g_vfx_n++; }
    mfx_unlock();
}

void bsdr_mediafx_video_remove_owner(void *user) { bsdr_mediafx_video_add(NULL, user, 0); }

int bsdr_mediafx_apply_video(uint8_t *y, int y_stride, uint8_t *uv, int uv_stride, int width, int height) {
    if (g_vfx_n == 0 || !y || width <= 0 || height <= 0) return 0;   /* cheap fast-path */
    mfx_lock();
    /* apply in ascending order (selection over a tiny list; the chain is applied under the lock so an
     * unload can't free a plugin mid-frame — same discipline as the audio effect) */
    int applied = 0;
    for (int pass = 0; pass < g_vfx_n; pass++) {
        int best = -1;
        for (int i = 0; i < g_vfx_n; i++)
            if (!(applied & (1 << i)) && (best < 0 || g_vfx[i].order < g_vfx[best].order)) best = i;
        if (best < 0) break;
        applied |= (1 << best);
        if (g_vfx[best].fn) g_vfx[best].fn(g_vfx[best].user, y, y_stride, uv, uv_stride, width, height);
    }
    int n = g_vfx_n;
    mfx_unlock();
    return n;
}

/* ---- dim-changing video-source transform (2D->3D) --------------------------------------------- */
static bsdr_video_src_fx g_vsrc;        /* copied by value; g_vsrc_set gates it */
static int               g_vsrc_set = 0;

void bsdr_mediafx_set_video_src(const bsdr_video_src_fx *fx) {
    if (!g_lock) g_lock = bsdr_mutex_new();
    mfx_lock();
    if (fx && fx->dims && fx->apply) { g_vsrc = *fx; g_vsrc_set = 1; }
    else { g_vsrc_set = 0; g_vsrc.dims = NULL; g_vsrc.apply = NULL; g_vsrc.user = NULL; }
    mfx_unlock();
}

int bsdr_mediafx_video_src_active(void) { return g_vsrc_set; }

int bsdr_mediafx_video_src_dims(int in_w, int in_h, int *out_w, int *out_h) {
    if (!g_vsrc_set) return 0;                       /* cheap fast-path */
    mfx_lock();
    int claimed = 0;
    if (g_vsrc_set && g_vsrc.dims) claimed = g_vsrc.dims(g_vsrc.user, in_w, in_h, out_w, out_h) ? 1 : 0;
    mfx_unlock();
    return claimed;
}

int bsdr_mediafx_apply_video_src(const uint8_t *sy, int sys, const uint8_t *suv, int suvs, int in_w, int in_h,
                                 uint8_t *dy, int dys, uint8_t *duv, int duvs, int out_w, int out_h) {
    if (!g_vsrc_set) return 0;
    mfx_lock();
    int applied = 0;
    if (g_vsrc_set && g_vsrc.apply) {
        g_vsrc.apply(g_vsrc.user, sy, sys, suv, suvs, in_w, in_h, dy, dys, duv, duvs, out_w, out_h);
        applied = 1;
    }
    mfx_unlock();
    return applied;
}

/* ---- depth estimator (2d-3d plugin; host-rendered stereo, e.g. Android GL) ---------------------- */
static bsdr_depth_fx_fn g_depth_fn = NULL;
static void            *g_depth_user = NULL;

void bsdr_mediafx_set_depth(bsdr_depth_fx_fn fn, void *user) {
    if (!g_lock) g_lock = bsdr_mutex_new();
    mfx_lock();
    g_depth_fn = fn;
    g_depth_user = user;
    mfx_unlock();
}

int bsdr_mediafx_depth_active(void) { return g_depth_fn != NULL; }

int bsdr_mediafx_apply_depth(int tier, const uint8_t *gray, int w, int h, float *out) {
    if (!g_depth_fn || !gray || !out || w <= 0 || h <= 0) return 0;
    mfx_lock();
    int ok = 0;
    if (g_depth_fn) ok = (g_depth_fn(g_depth_user, tier, gray, w, h, out) == 0) ? 1 : 0;
    mfx_unlock();
    return ok;
}

/* ---- packed-RGB face-swap interface (faceswap plugin; host-supplied RGB, e.g. Android GL) ------- */
static bsdr_face_fx g_face;
static int          g_face_set = 0;

void bsdr_mediafx_set_face(const bsdr_face_fx *fx) {
    if (!g_lock) g_lock = bsdr_mutex_new();
    mfx_lock();
    if (fx && fx->process) { g_face = *fx; g_face_set = 1; }
    else { g_face_set = 0; g_face.process = NULL; g_face.set_source = NULL; g_face.user = NULL; }
    mfx_unlock();
}

int bsdr_mediafx_face_active(void) { return g_face_set; }

int bsdr_mediafx_face_process(uint8_t *rgb, int w, int h) {
    if (!g_face_set || !rgb || w <= 0 || h <= 0) return -1;
    mfx_lock();
    int n = (g_face_set && g_face.process) ? g_face.process(g_face.user, rgb, w, h) : -1;
    mfx_unlock();
    return n;
}

int bsdr_mediafx_face_set_source(const uint8_t *rgb, int w, int h) {
    if (!g_face_set || !rgb || w <= 0 || h <= 0) return -1;
    mfx_lock();
    int r = (g_face_set && g_face.set_source) ? g_face.set_source(g_face.user, rgb, w, h) : -1;
    mfx_unlock();
    return r;
}
