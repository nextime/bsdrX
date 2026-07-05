/* model_store.h — depth-model catalog, on-demand download, and manual zip import.
 *
 * The in-process depth tiers (depth.h) need an .onnx model file. Models are NOT bundled (99 MB..
 * 1.4 GB); they live in a per-OS cache dir and are acquired two ways:
 *   - auto-download from the catalog URL (streaming HTTPS + SHA-256), or
 *   - imported from a zip we distribute (one per model, plus an all-three zip), for offline use.
 * Both land under canonical names in the same cache. The catalog is the single source of truth for
 * filename / URL / checksum / model input size / preprocessing family, shared by depth_onnx.c
 * (input size + preprocess) and the download/import verifier.
 *
 * Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
 */
#ifndef BSDR_MODEL_STORE_H
#define BSDR_MODEL_STORE_H
#include <stddef.h>

typedef struct {
    int          tier;        /* 1=cpu 2=gpu 3=hi */
    const char  *name;        /* canonical model name (also the manifest key) */
    const char  *filename;    /* cache filename, e.g. "depth-anything-v2-small.onnx" */
    const char  *url;         /* upstream download URL (https) */
    const char  *sha256;      /* lowercase hex; "" if not pinned yet */
    int          input_size;  /* square model input edge (e.g. 518 / 384) */
    int          approx_mb;   /* rough download size (MB), for the web-UI hint */
    /* Preprocess (all models: gray->3ch RGB, scale to [0,1], then per-channel (v-mean)/std, NCHW).
     * DA-V2 = ImageNet mean/std; HF/Xenova DPT (MiDaS) = 0.5/0.5; a "[0,1]-baked" export = 0/1. */
    float        mean[3];
    float        std[3];
} bsdr_model_info;

/* Catalog lookup for a tier (1/2/3); NULL for an invalid tier. */
const bsdr_model_info *bsdr_model_for_tier(int tier);

/* The per-OS model cache directory (created if missing). Returns 0 on success. */
int bsdr_model_dir(char *out, size_t cap);

/* Ensure the model for `tier` is present in the cache, downloading it if `allow_download` and it's
 * missing. On success returns 0 and writes the absolute model path into `path`. Returns -1 if the
 * model is absent and can't be obtained (caller should tell the user to import the zip). */
int bsdr_model_resolve(int tier, int allow_download, char *path, size_t cap);

/* Import models from a distributed zip (per-model or the all-three zip). Extracts each .onnx,
 * verifies it against the catalog SHA-256 (via the in-zip manifest), and places it in the cache
 * under the canonical filename. Returns the number of models imported, or -1 on error. */
int bsdr_model_import_zip(const char *zip_path);

/* 1 if the model file for `tier` already exists (and matches, if the sha is pinned) in the cache. */
int bsdr_model_present(int tier);

/* Kick off a BACKGROUND (async, non-blocking) download of the tier's model into the cache. Idempotent:
 * a no-op if the model is already present or a download is already running (only one at a time).
 * Progress is observable via bsdr_model_download_state(). Returns 0 if started/queued/already-present,
 * -1 on an invalid tier or a tier with no download URL (import the zip instead). */
int bsdr_model_download_start(int tier);

/* Snapshot of the current (or most recent) background download, for the web-UI status. */
typedef struct {
    int   active;     /* 1 while a download is in flight */
    int   tier;       /* tier being (or last) downloaded; 0 if none yet */
    long  done;       /* bytes fetched so far */
    long  total;      /* content-length, or 0 if the server didn't send one */
    int   pct;        /* 0..100, or -1 when total is unknown */
    int   ok;         /* 1 if the last finished download succeeded */
    char  name[64];   /* model name */
    char  err[96];    /* last error text, "" if none */
} bsdr_model_dl;
void bsdr_model_download_state(bsdr_model_dl *out);

#endif /* BSDR_MODEL_STORE_H */
