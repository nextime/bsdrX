/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* faceswap.h — realtime face swap / deepfake onto the streamed video, GPU-permitting.
 *
 * Pipeline (insightface-compatible ONNX models, run in-process via ONNX Runtime):
 *   1. SCRFD detector  -> face boxes + 5 keypoints
 *   2. ArcFace (w600k) -> a 512-d identity embedding of the operator's SOURCE image
 *   3. inswapper_128   -> paste that identity onto each detected face in the frame
 * The three .onnx are user-supplied (insightface is non-commercial → never bundled): dropped into the
 * faceswap model dir or imported via the web UI, exactly like the depth models. Without them, or
 * without a source image, or if ORT isn't built in, open() returns NULL and the caller streams the
 * untouched frame. GPU EP (CUDA/DirectML/CoreML/NNAPI) when the tier asks; CPU fallback otherwise.
 */
#ifndef BSDR_FACESWAP_H
#define BSDR_FACESWAP_H

#include <stdint.h>

typedef struct bsdr_faceswap bsdr_faceswap;

/* Load the detector/recognizer/swapper from `model_dir` (files: det_10g.onnx, w600k_r50.onnx,
 * inswapper_128.onnx). use_gpu selects the accelerated EP. Returns NULL if a model is missing / ORT
 * is unavailable. */
bsdr_faceswap *bsdr_faceswap_open(const char *model_dir, int use_gpu);

/* Set the identity to paste, from a source RGB image (packed R,G,B rows). Detects the largest face,
 * aligns it, and computes+caches the swap latent. Returns 0 on success, -1 if no face was found. */
int bsdr_faceswap_set_source_rgb(bsdr_faceswap *fs, const uint8_t *rgb, int w, int h);

/* 1 if a source identity is set (so process() will do something). */
int bsdr_faceswap_ready(const bsdr_faceswap *fs);

/* Swap every detected face in the packed-RGB frame IN PLACE to the source identity. Returns the
 * number of faces swapped (>=0), or -1 on error. A no-op (0) if no source is set. */
int bsdr_faceswap_process_rgb(bsdr_faceswap *fs, uint8_t *rgb, int w, int h);

const char *bsdr_faceswap_status(const bsdr_faceswap *fs);

/* Opt-in (P4.5): run face DETECTION only every n frames (n>=2), reusing the last boxes/keypoints in
 * between; the swap still runs every frame. n<=1 (default) = detect every frame. ~halves faceswap
 * CPU at the cost of slight tracking lag on fast head motion. */
void bsdr_faceswap_set_detect_every(bsdr_faceswap *fs, int n);

void bsdr_faceswap_close(bsdr_faceswap *fs);

#endif /* BSDR_FACESWAP_H */
