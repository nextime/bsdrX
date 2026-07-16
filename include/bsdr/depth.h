/* depth.h — in-process neural monocular depth estimation for the 2D->3D pipeline.
 *
 * Complements threed.c's built-in heuristic (FAST) and external co-process (AI, the depth-helper
 * over the BSDD pipe). This module runs the model INSIDE bsdrX via ONNX Runtime, in three
 * performance tiers the operator picks — equivalent (not identical) across Linux/Windows/macOS/
 * Android:
 *   CPU (1) — Depth-Anything-V2-Small (Apache-2.0), CPU / XNNPACK. Old laptops.
 *   GPU (2) — MiDaS DPT-Hybrid (MIT), a small GPU (DirectML / CoreML / NNAPI / CUDA).
 *   HI  (3) — MiDaS DPT-BEiT-Large (MIT), a serious GPU (CUDA+TensorRT / DirectML / CoreML).
 * Models are acquired on demand (download + checksum) or imported from a distributed zip; see
 * model_store.h. Compiled only when BSDR_HAVE_ONNX is defined; otherwise every entry point is a
 * no-op so callers still link and fall back to the co-process / heuristic.
 *
 * Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
 */
#ifndef BSDR_DEPTH_H
#define BSDR_DEPTH_H
#include <stdint.h>
#include <stddef.h>

typedef enum {
    BSDR_DEPTH_AUTO = 0,   /* pick the best tier the machine can run */
    BSDR_DEPTH_CPU  = 1,   /* Depth-Anything-V2-Small, CPU / XNNPACK */
    BSDR_DEPTH_GPU  = 2,   /* MiDaS DPT-Hybrid, GPU execution provider */
    BSDR_DEPTH_HI   = 3,   /* MiDaS DPT-BEiT-Large, best GPU execution provider */
} bsdr_depth_tier;

typedef struct bsdr_depth bsdr_depth;

/* Open an in-process estimator for `tier` (AUTO resolves to the best available). Resolves the
 * model via model_store (downloading/importing if needed), selects an execution provider, and
 * loads the ONNX session. Returns NULL when ONNX is unavailable, the model can't be obtained, or
 * the session fails to load — the caller then falls back to the co-process / heuristic. */
bsdr_depth *bsdr_depth_open(bsdr_depth_tier tier);

/* Estimate depth for a `w`x`h` grayscale image (row-major, `w` bytes per row). Writes `w*h`
 * floats into `out`, range 0..1 with larger = nearer (same convention as the BSDD helper). The
 * grid is small (<=256 wide) so this stays real-time-ish. Returns 0 on success, -1 on failure. */
int bsdr_depth_infer(bsdr_depth *d, const uint8_t *gray, int w, int h, float *out);

void bsdr_depth_close(bsdr_depth *d);

/* Human-readable status for logs / the web panel, e.g. "cpu:depth-anything-v2-small (xnnpack)".
 * Never NULL (returns "unavailable" for a NULL handle). */
const char *bsdr_depth_status(bsdr_depth *d);

/* Parse a tier name ("auto"|"cpu"|"gpu"|"hi"); BSDR_DEPTH_AUTO for NULL/unknown. */
bsdr_depth_tier bsdr_depth_tier_parse(const char *s);
const char *bsdr_depth_tier_name(bsdr_depth_tier t);

/* 1 if this build has ONNX support compiled in (BSDR_HAVE_ONNX), else 0. */
int bsdr_depth_available(void);

/* Model provider — the embedder supplies the engine its per-tier model file + preprocessing params, so
 * this module never links the core model store directly (it now lives in the 2d-3d plugin). The 2d-3d
 * plugin sets it to thin wrappers over host services; the Android JNI sets it to the in-JNI model store.
 * Set once before bsdr_depth_open (a NULL/absent provider makes open() fail -> heuristic fallback). */
typedef struct {
    /* per-tier params: model display name, square input edge, per-channel mean/std. Return 0 on success. */
    int (*params)(int tier, char *name, size_t name_cap, int *input_size, float mean[3], float std[3]);
    /* resolve the cached model path (allow_download: kick a background fetch if missing). 0 on success. */
    int (*resolve)(int tier, int allow_download, char *path, size_t cap);
    /* start a background download of the tier's model. 0 if started/queued/already present. */
    int (*download_start)(int tier);
} bsdr_depth_provider;
void bsdr_depth_set_provider(const bsdr_depth_provider *p);

/* 1 if the tier's model is already cached (resolve succeeds without a download), via the provider. Lets
 * the SBS synth tell "download in flight" (keep retrying) from "present but won't load" (give up). */
int bsdr_depth_model_present(int tier);

#endif /* BSDR_DEPTH_H */
