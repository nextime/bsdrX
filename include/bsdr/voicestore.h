/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* voicestore.h — model store for the AI voice changer (RVC voice conversion).
 *
 * Two kinds of assets, both under <model cache>/voice (see model_store.h for the cache root):
 *
 *   1. ENGINE BASE MODELS (fixed catalog, shared by every voice): the content encoder (ContentVec)
 *      and the pitch extractor (RMVPE). Downloaded once, on demand, with progress — like the depth
 *      store. These are the same across platforms (ONNX Runtime runs them on CPU/NNAPI/CoreML/CUDA).
 *
 *   2. VOICE LIBRARY (dynamic, user-managed): one ONNX file per target voice (an RVC generator export)
 *      under <cache>/voice/voices/<id>.onnx, plus an optional <id>.json sidecar (display name, target
 *      sample rate, whether it needs F0, speaker id, model version). Voices are added three ways from
 *      the web UI: upload a local file, download from a URL, or pick from the built-in online catalog.
 *
 * All weights are USER-SUPPLIED / user-downloaded (like the face-swap inswapper model): bsdrX ships no
 * voice weights, so distribution stays clean. Everything here is cross-platform C over bsdr_http_download
 * + miniz + OpenSSL, identical on Linux / Windows / macOS / Android.
 */
#ifndef BSDR_VOICESTORE_H
#define BSDR_VOICESTORE_H

#include <stddef.h>
#include "bsdr/model_store.h"   /* bsdr_model_dl (shared download-progress shape) */

/* ---- engine base models (content encoder + pitch extractor) ---------------------------------- */
typedef enum { BSDR_VBASE_CONTENT = 0, BSDR_VBASE_RMVPE = 1, BSDR_VBASE_COUNT = 2 } bsdr_vbase;

typedef struct {
    const char *name;        /* canonical name / manifest key */
    const char *filename;    /* cache filename, e.g. "contentvec.onnx" */
    const char *url;         /* upstream https URL ("" = import/supply only) */
    const char *sha256;      /* lowercase hex, "" if unpinned */
    int         approx_mb;   /* rough size for the web-UI hint */
} bsdr_vbase_info;

const bsdr_vbase_info *bsdr_voice_base_info(bsdr_vbase which);
/* <model cache>/voice (created if missing). Returns 0 on success. */
int  bsdr_voice_dir(char *out, size_t cap);
/* Absolute path of a base model in the cache. Returns 0 on success. */
int  bsdr_voice_base_path(bsdr_vbase which, char *out, size_t cap);
/* 1 if the base model file is present (and matches its sha, if pinned). */
int  bsdr_voice_base_present(bsdr_vbase which);
/* 1 if BOTH base models are present (the engine needs the content encoder; RMVPE only for GPU F0). */
int  bsdr_voice_base_ready(void);
/* Kick off a BACKGROUND download of any missing base model. Idempotent; one at a time. Returns 0 if
 * started/complete, -1 if it couldn't start. Progress via bsdr_voice_download_state(). */
int  bsdr_voice_base_download_start(void);
/* Import base models found in a local zip (canonical filenames) into the cache. Returns count or -1. */
int  bsdr_voice_base_import_zip(const char *zip_path);
/* Snapshot of the voice download channel (base models AND voices share it; one at a time). */
void bsdr_voice_download_state(bsdr_model_dl *out);

/* ---- voice library (one ONNX per target voice) ----------------------------------------------- */
typedef struct {
    char id[64];             /* filesystem id (basename w/o extension) */
    char name[96];           /* display name (from the sidecar, or the id) */
    int  sample_rate;        /* target output SR (32000/40000/48000), 0 = unknown/default 40000 */
    int  needs_f0;           /* 1 if the generator takes pitch inputs (RVC "f0" models); default 1 */
    int  version;            /* RVC model version hint (1/2), 0 = auto-detect from I/O */
    long size_bytes;         /* .onnx size on disk */
} bsdr_voice_entry;

/* List voices in the library. Fills up to `cap` entries, returns the count (>=0), or -1 on error. */
int  bsdr_voice_list(bsdr_voice_entry *out, int cap);
/* Absolute .onnx path for a voice id. Returns 0 on success, -1 if not found. */
int  bsdr_voice_path(const char *id, char *out, size_t cap);
/* Metadata for one voice id (reads the sidecar if present). Returns 0 on success. */
int  bsdr_voice_get(const char *id, bsdr_voice_entry *out);
/* Add a voice by COPYING a local .onnx file into the library under `id` (sanitized). An optional
 * display `name` and target `sample_rate`/`needs_f0` write a sidecar. Returns 0 on success. */
int  bsdr_voice_add_file(const char *src_path, const char *id, const char *name,
                         int sample_rate, int needs_f0);
/* Remove a voice (its .onnx + sidecar + any .index). Returns 0 on success. */
int  bsdr_voice_delete(const char *id);

/* Kick off a BACKGROUND download of a voice .onnx from `url` into the library under `id` (display
 * `name` optional). Idempotent; shares the single voice download channel. Returns 0 if started, -1
 * otherwise. Progress via bsdr_voice_download_state(); on success the voice appears in the library. */
int  bsdr_voice_download_start(const char *url, const char *id, const char *name);

/* ---- built-in online catalog (curated, permissive/user-downloadable voices) ------------------- */
typedef struct {
    const char *id;          /* library id to save under */
    const char *name;        /* display name */
    const char *url;         /* .onnx download URL */
    int         sample_rate; /* target SR */
    int         approx_mb;
} bsdr_voice_catalog_entry;

/* The built-in curated catalog (a small set of downloadable example voices). Returns the array and
 * writes the count. These are conveniences; arbitrary URLs work via bsdr_voice_download_start(). */
const bsdr_voice_catalog_entry *bsdr_voice_catalog(int *count);

#endif /* BSDR_VOICESTORE_H */
