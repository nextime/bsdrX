/*
 * bsdrX — media-effect registry unit test (voice/video plugin hook).
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>. GPLv3-or-later.
 */
#include "bsdr/mediafx.h"

#include <stdio.h>
#include <string.h>

static int   g_calls = 0;
static void *g_user_seen = NULL;

static void fx(void *user, int16_t *pcm, int frames, int rate, int channels) {
    g_calls++; g_user_seen = user;
    (void)rate;
    for (int i = 0; i < frames * (channels > 0 ? channels : 1); i++) pcm[i] = 4242;
}

/* video chain: each effect appends its owner's tag to a shared order-log so we can assert the
 * chain ran in ascending `order`. */
static char g_vlog[32];
static int  g_vn = 0;
static void vfx_append(void *user) { if (g_vn < (int)sizeof g_vlog - 1) g_vlog[g_vn++] = *(char *)user; g_vlog[g_vn] = 0; }
static void vfx_a(void *u, uint8_t *y, int ys, uint8_t *uv, int uvs, int w, int h) { (void)y;(void)ys;(void)uv;(void)uvs;(void)w;(void)h; vfx_append(u); }

/* video-source (2D->3D) stub: doubles the width (full SBS) and records that apply ran. */
static int  g_vsrc_apply = 0;
static int  vsrc_dims(void *user, int in_w, int in_h, int *ow, int *oh) { (void)user; *ow = in_w * 2; *oh = in_h; return 1; }
static void vsrc_apply(void *user, const uint8_t *sy, int sys, const uint8_t *suv, int suvs, int in_w, int in_h,
                       uint8_t *dy, int dys, uint8_t *duv, int duvs, int ow, int oh) {
    (void)user;(void)sy;(void)sys;(void)suv;(void)suvs;(void)in_w;(void)in_h;(void)dy;(void)dys;(void)duv;(void)duvs;(void)ow;(void)oh;
    g_vsrc_apply++;
}

int main(void) {
    int fails = 0;
    int16_t buf[16] = {0};

    /* nothing registered -> apply is a no-op returning 0 */
    if (bsdr_mediafx_apply_audio(buf, 16, 48000, 1) != 0) { fprintf(stderr, "FAIL: applied with no fx\n"); fails++; }
    if (buf[0] != 0) { fprintf(stderr, "FAIL: buffer touched with no fx\n"); fails++; }

    /* register -> apply runs the effect, returns 1, passes the user pointer */
    int marker = 7;
    bsdr_mediafx_set_audio(fx, &marker);
    if (bsdr_mediafx_apply_audio(buf, 16, 48000, 1) != 1) { fprintf(stderr, "FAIL: apply did not run the fx\n"); fails++; }
    if (g_calls != 1 || buf[0] != 4242 || g_user_seen != &marker) { fprintf(stderr, "FAIL: fx not invoked correctly\n"); fails++; }

    /* clear -> back to no-op */
    int16_t b2[16] = {0};
    bsdr_mediafx_set_audio(NULL, NULL);
    if (bsdr_mediafx_apply_audio(b2, 16, 48000, 1) != 0 || b2[0] != 0) { fprintf(stderr, "FAIL: fx still active after clear\n"); fails++; }

    /* ---- video chain -------------------------------------------------------------------------- */
    uint8_t y[4] = {0}, uv[2] = {0};

    /* nothing registered -> apply is a cheap no-op returning 0 */
    if (bsdr_mediafx_apply_video(y, 2, uv, 2, 2, 2) != 0) { fprintf(stderr, "FAIL: video applied with no fx\n"); fails++; }

    /* register two effects out of order (order 20 then order 10) -> chain runs 10 before 20 */
    char tag_face = 'F', tag_3d = 'D';
    bsdr_mediafx_video_add(vfx_a, &tag_3d,   20);   /* late */
    bsdr_mediafx_video_add(vfx_a, &tag_face, 10);   /* early */
    g_vn = 0; g_vlog[0] = 0;
    if (bsdr_mediafx_apply_video(y, 2, uv, 2, 2, 2) != 2) { fprintf(stderr, "FAIL: video chain count != 2\n"); fails++; }
    if (strcmp(g_vlog, "FD") != 0) { fprintf(stderr, "FAIL: video chain order = '%s' (want 'FD')\n", g_vlog); fails++; }

    /* re-add same owner (keyed by user) -> replaces, not duplicates */
    bsdr_mediafx_video_add(vfx_a, &tag_face, 30);   /* now face is LATE */
    g_vn = 0; g_vlog[0] = 0;
    if (bsdr_mediafx_apply_video(y, 2, uv, 2, 2, 2) != 2) { fprintf(stderr, "FAIL: video chain count != 2 after re-add\n"); fails++; }
    if (strcmp(g_vlog, "DF") != 0) { fprintf(stderr, "FAIL: re-add order = '%s' (want 'DF')\n", g_vlog); fails++; }

    /* remove one owner -> only the other remains */
    bsdr_mediafx_video_remove_owner(&tag_3d);
    g_vn = 0; g_vlog[0] = 0;
    if (bsdr_mediafx_apply_video(y, 2, uv, 2, 2, 2) != 1) { fprintf(stderr, "FAIL: video chain count != 1 after remove\n"); fails++; }
    if (strcmp(g_vlog, "F") != 0) { fprintf(stderr, "FAIL: after remove = '%s' (want 'F')\n", g_vlog); fails++; }

    /* remove the last -> back to no-op */
    bsdr_mediafx_video_remove_owner(&tag_face);
    if (bsdr_mediafx_apply_video(y, 2, uv, 2, 2, 2) != 0) { fprintf(stderr, "FAIL: video chain not empty after removes\n"); fails++; }

    /* ---- video-source (2D->3D) transform ------------------------------------------------------ */
    int ow2 = 0, oh2 = 0;
    /* nothing registered -> dims declines, apply is a no-op */
    if (bsdr_mediafx_video_src_active() != 0)                          { fprintf(stderr, "FAIL: vsrc active before register\n"); fails++; }
    if (bsdr_mediafx_video_src_dims(1920, 1080, &ow2, &oh2) != 0)      { fprintf(stderr, "FAIL: vsrc dims claimed with none\n"); fails++; }
    if (bsdr_mediafx_apply_video_src(y,2,uv,2,2,2, y,4,uv,4,4,2) != 0) { fprintf(stderr, "FAIL: vsrc applied with none\n"); fails++; }

    /* register -> dims claims + declares 2x width, apply runs and reports handled */
    bsdr_video_src_fx vs = { .dims = vsrc_dims, .apply = vsrc_apply, .user = NULL };
    bsdr_mediafx_set_video_src(&vs);
    if (bsdr_mediafx_video_src_active() != 1)                          { fprintf(stderr, "FAIL: vsrc not active after register\n"); fails++; }
    if (bsdr_mediafx_video_src_dims(1920, 1080, &ow2, &oh2) != 1 || ow2 != 3840 || oh2 != 1080) {
        fprintf(stderr, "FAIL: vsrc dims = %dx%d (want 3840x1080)\n", ow2, oh2); fails++; }
    g_vsrc_apply = 0;
    if (bsdr_mediafx_apply_video_src(y,2,uv,2,2,2, y,4,uv,4,4,2) != 1 || g_vsrc_apply != 1) {
        fprintf(stderr, "FAIL: vsrc apply not run\n"); fails++; }

    /* clear -> back to declining */
    bsdr_mediafx_set_video_src(NULL);
    if (bsdr_mediafx_video_src_active() != 0)                          { fprintf(stderr, "FAIL: vsrc still active after clear\n"); fails++; }
    if (bsdr_mediafx_apply_video_src(y,2,uv,2,2,2, y,4,uv,4,4,2) != 0) { fprintf(stderr, "FAIL: vsrc applied after clear\n"); fails++; }

    if (fails == 0) printf("PASS: test_mediafx\n");
    return fails ? 1 : 0;
}
