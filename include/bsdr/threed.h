/* threed.h — real-time 2D->3D (side-by-side stereo) for the encode pipeline.
 *
 * The Bigscreen remote screen can display a side-by-side (SBS) 3D frame: left half = left eye,
 * right half = right eye, each stretched back to full width per eye. This module synthesises that
 * SBS pair from an ordinary 2D NV12 frame via depth-image-based rendering (a per-pixel horizontal
 * shift proportional to an estimated depth).
 *
 * Two depth sources, so it runs on anything:
 *   FAST  — a built-in heuristic (vertical gradient + normalised luma). No dependencies, a few
 *           passes over the frame; comfortable on old laptops.
 *   AI    — an external depth-estimator co-process (e.g. a MiDaS ONNX wrapper) the operator points
 *           at. bsdrX pipes it small downscaled frames and reads back a depth map, at a reduced
 *           rate with temporal reuse so even the AI path stays light. Falls back to FAST if the
 *           helper is missing, stalls, or dies.
 *
 * Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
 */
#ifndef BSDR_THREED_H
#define BSDR_THREED_H
#include <stdint.h>

typedef enum {
    BSDR_3D_OFF  = 0,
    BSDR_3D_FAST = 1,   /* built-in heuristic depth (light) */
    BSDR_3D_AI   = 2,   /* external depth-estimator co-process (falls back to FAST) */
} bsdr_threed_mode;

typedef struct {
    bsdr_threed_mode mode;
    int deepness;      /* 0..100 "depth amount": max eye disparity as a fraction of frame width */
    int convergence;   /* -50..50: move the zero-parallax (screen) plane nearer/farther for comfort */
    int swap;          /* 1 = pack right|left instead of left|right (headset eye-order fix) */
    int tier;          /* AI mode: in-process depth tier 0=none(use ai_cmd) 1=cpu 2=gpu 3=hi (see depth.h) */
    char ai_cmd[256];  /* AI mode: shell command of the external depth helper (operator-configured) */
} bsdr_threed_config;

typedef struct bsdr_threed bsdr_threed;

/* Create for a SOURCE NV12 size. The synthesised SBS output is half-SBS (two half-width squished
 * eyes) at src_w x src_h — the ONLY format the Quest's screen shows as 3D (a 2x-wide frame just makes
 * a double-wide screen). Render at higher src_w/src_h for sharper eyes. NULL on OOM / mode OFF. */
bsdr_threed *bsdr_threed_create(int src_w, int src_h, const bsdr_threed_config *cfg);

/* Width the encoder must be sized to (== src_w; the packed SBS output equals the source frame). */
int bsdr_threed_out_width(bsdr_threed *t);

/* Synthesise the SBS frame: read the source NV12 (s*), write the packed SBS NV12 (d*). Dest luma is
 * out_width x src_h; dest chroma out_width bytes/row x src_h/2. Safe no-op if t is NULL. */
void bsdr_threed_apply_nv12(bsdr_threed *t,
                            const uint8_t *sy, int sy_stride, const uint8_t *suv, int suv_stride,
                            uint8_t *dy, int dy_stride, uint8_t *duv, int duv_stride);

void bsdr_threed_close(bsdr_threed *t);

/* "off" | "fast" | "ai"  <->  enum. parse() returns BSDR_3D_OFF for NULL/unknown. */
bsdr_threed_mode bsdr_threed_mode_parse(const char *s);
const char *bsdr_threed_mode_name(bsdr_threed_mode m);

#endif /* BSDR_THREED_H */
