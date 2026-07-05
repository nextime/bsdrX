/* depth_onnx.c — in-process monocular depth via ONNX Runtime (C API). See depth.h.
 *
 * Compiled only when BSDR_HAVE_ONNX is defined. Without it, the stubs at the bottom keep callers
 * linking (bsdr_depth_open returns NULL) so the co-process / heuristic paths take over.
 *
 * Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
 */
#include "bsdr/depth.h"
#include "bsdr/model_store.h"
#include "bsdr/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bsdr_depth_tier bsdr_depth_tier_parse(const char *s) {
    if (!s) return BSDR_DEPTH_AUTO;
    if (!strcmp(s, "cpu")) return BSDR_DEPTH_CPU;
    if (!strcmp(s, "gpu")) return BSDR_DEPTH_GPU;
    if (!strcmp(s, "hi"))  return BSDR_DEPTH_HI;
    return BSDR_DEPTH_AUTO;
}
const char *bsdr_depth_tier_name(bsdr_depth_tier t) {
    switch (t) { case BSDR_DEPTH_CPU: return "cpu"; case BSDR_DEPTH_GPU: return "gpu";
                 case BSDR_DEPTH_HI: return "hi"; default: return "auto"; }
}

#ifdef BSDR_HAVE_ONNX
#include "onnxruntime_c_api.h"
#ifdef _WIN32
#  include <windows.h>   /* MultiByteToWideChar for the ORTCHAR_T (wchar_t) model path */
#endif
#ifdef __ANDROID__
#  include <dlfcn.h>     /* dlsym the NNAPI EP factory (dedicated, not the string-name API) */
#endif
/* CoreML / NNAPI / XNNPACK are attached via ORT's string-name EP API (resolved at runtime, so no
 * link-time symbol dependency — the desktop macOS ORT ships the CoreML header but not always the
 * symbol). CUDA / DirectML still use dedicated factory functions and are compiled only when built
 * against that ORT (BSDR_ONNX_CUDA / BSDR_ONNX_DML), whose platform headers are then present. */
#if defined(_WIN32) && defined(BSDR_ONNX_DML)
#  include "dml_provider_factory.h"
#endif
#if defined(BSDR_ONNX_CUDA)
#  include "cuda_provider_factory.h"
#endif

int bsdr_depth_available(void) { return 1; }

struct bsdr_depth {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtSession *sess;
    OrtMemoryInfo *mem;
    char *in_name;               /* owned (allocator-freed at close) */
    char *out_name;
    const bsdr_model_info *mi;    /* catalog entry: input_size + preprocess family */
    int S;                       /* model input edge */
    float *in_buf;               /* [1,3,S,S] NCHW */
    char status[128];
    const char *ep;              /* EP label for status */
};

/* Bilinear sample of a w x h uint8 image at fractional (fx,fy), clamped. */
static float sample_u8(const uint8_t *img, int w, int h, float fx, float fy) {
    if (fx < 0) fx = 0; else if (fx > w - 1) fx = (float)(w - 1);
    if (fy < 0) fy = 0; else if (fy > h - 1) fy = (float)(h - 1);
    int x0 = (int)fx, y0 = (int)fy; float ax = fx - x0, ay = fy - y0;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    float a = img[y0 * w + x0], b = img[y0 * w + x1], c = img[y1 * w + x0], d = img[y1 * w + x1];
    return (a * (1 - ax) + b * ax) * (1 - ay) + (c * (1 - ax) + d * ax) * ay;
}
/* Bilinear sample of a w x h float image. */
static float sample_f(const float *img, int w, int h, float fx, float fy) {
    if (fx < 0) fx = 0; else if (fx > w - 1) fx = (float)(w - 1);
    if (fy < 0) fy = 0; else if (fy > h - 1) fy = (float)(h - 1);
    int x0 = (int)fx, y0 = (int)fy; float ax = fx - x0, ay = fy - y0;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    float a = img[y0 * w + x0], b = img[y0 * w + x1], c = img[y1 * w + x0], d = img[y1 * w + x1];
    return (a * (1 - ax) + b * ax) * (1 - ay) + (c * (1 - ax) + d * ax) * ay;
}

/* log + release an OrtStatus; returns 1 if it was an error. */
static int ort_bad(const OrtApi *ort, OrtStatus *st, const char *what) {
    if (!st) return 0;
    BSDR_WARN("bsdr.depth", "%s: %s", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return 1;
}
/* release an OrtStatus we don't act on (config/getter calls that "can't" fail here). */
static void ort_drop(const OrtApi *ort, OrtStatus *st) { if (st) ort->ReleaseStatus(st); }

/* Attach the best execution provider for `tier`, most-accelerated first, always ending at the
 * default CPU provider. GPU tiers try the platform accelerator (CoreML / NNAPI / DirectML) then
 * CUDA (NVIDIA, only when built against a CUDA-enabled ORT); a missing/failed EP silently falls
 * through. Every attach that isn't compiled/available for this platform is simply skipped, so the
 * function compiles everywhere and degrades to CPU. Sets d->ep for the status string. */
static void select_ep(bsdr_depth *d, bsdr_depth_tier tier) {
    const OrtApi *ort = d->ort;
    d->ep = "cpu";
    int want_gpu = (tier == BSDR_DEPTH_GPU || tier == BSDR_DEPTH_HI);

    if (want_gpu) {
#if defined(__ANDROID__)
        /* NNAPI needs its dedicated factory (the string-name API doesn't cover it); resolve at runtime
         * so a build lacking it falls through to CPU/XNNPACK. */
        typedef OrtStatus *(*nnapi_fn)(OrtSessionOptions *, uint32_t);
        nnapi_fn nn = (nnapi_fn)dlsym(RTLD_DEFAULT, "OrtSessionOptionsAppendExecutionProvider_Nnapi");
        if (nn) { OrtStatus *st = nn(d->opts, 0); if (!st) { d->ep = "nnapi"; return; } ort->ReleaseStatus(st); }
#endif
        /* Native accelerator via the string-name API (no link-time symbol dep). */
        const char *gpu_ep = NULL;
#if defined(__APPLE__)
        gpu_ep = "CoreML";
#endif
        if (gpu_ep) {
            OrtStatus *st = ort->SessionOptionsAppendExecutionProvider(d->opts, gpu_ep, NULL, NULL, 0);
            if (!st) { d->ep = gpu_ep; return; } ort->ReleaseStatus(st);
        }
#if defined(_WIN32) && defined(BSDR_ONNX_DML)
        OrtStatus *dst = OrtSessionOptionsAppendExecutionProvider_DML(d->opts, 0);
        if (!dst) { d->ep = "dml"; return; } ort->ReleaseStatus(dst);
#endif
#if defined(BSDR_ONNX_CUDA)
        OrtCUDAProviderOptions cuda; memset(&cuda, 0, sizeof cuda);
        OrtStatus *cst = ort->SessionOptionsAppendExecutionProvider_CUDA(d->opts, &cuda);
        if (!cst) { d->ep = "cuda"; return; } ort->ReleaseStatus(cst);
#endif
    }
    /* CPU tier, or GPU fallback: XNNPACK where the ORT build has it (ARM/mobile/some x86), else the
     * default CPU provider. Pass no options — key names differ per EP. */
    OrtStatus *st = ort->SessionOptionsAppendExecutionProvider(d->opts, "XNNPACK", NULL, NULL, 0);
    if (!st) d->ep = "xnnpack"; else ort->ReleaseStatus(st);
}

bsdr_depth *bsdr_depth_open(bsdr_depth_tier tier) {
    if (tier == BSDR_DEPTH_AUTO) {
        /* macOS/Android always have a built-in GPU EP (CoreML/NNAPI), so the small-GPU tier is a
         * safe default there. Elsewhere stay on CPU unless the user explicitly asks for gpu/hi
         * (Windows-DML / NVIDIA-CUDA availability is probed in Phase C). */
#if defined(__APPLE__) || defined(__ANDROID__)
        tier = BSDR_DEPTH_GPU;
#else
        tier = BSDR_DEPTH_CPU;
#endif
    }
    const bsdr_model_info *mi = bsdr_model_for_tier((int)tier);
    if (!mi) { BSDR_WARN("bsdr.depth", "no model for tier %d", tier); return NULL; }

    /* Use only the cached model here (never block this call on a multi-hundred-MB download). If it's
     * missing, kick a background download — progress shows in the web UI — and fail for now so the
     * caller falls back to the heuristic; a later open() retry picks the model up once it lands. */
    char model_path[1024];
    if (bsdr_model_resolve((int)tier, /*allow_download=*/0, model_path, sizeof model_path) != 0) {
        if (bsdr_model_download_start((int)tier) == 0)
            BSDR_INFO("bsdr.depth", "tier %s: model '%s' not cached — downloading in background "
                      "(heuristic depth meanwhile)", bsdr_depth_tier_name(tier), mi->name);
        else
            BSDR_WARN("bsdr.depth", "tier %s: model '%s' unavailable (import the model zip)",
                      bsdr_depth_tier_name(tier), mi->name);
        return NULL;
    }

    bsdr_depth *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!d->ort) { free(d); return NULL; }
    const OrtApi *ort = d->ort;
    d->mi = mi; d->S = mi->input_size;

    OrtAllocator *alloc = NULL;
    if (ort_bad(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "bsdr", &d->env), "CreateEnv")) goto fail;
    if (ort_bad(ort, ort->CreateSessionOptions(&d->opts), "CreateSessionOptions")) goto fail;
    ort_drop(ort, ort->SetIntraOpNumThreads(d->opts, 0));   /* 0 = let ORT pick */
    ort_drop(ort, ort->SetSessionGraphOptimizationLevel(d->opts, ORT_ENABLE_ALL));
    select_ep(d, tier);
    /* ORT's model path is ORTCHAR_T*: wchar_t on Windows, char on POSIX. Convert the UTF-8 path. */
#ifdef _WIN32
    wchar_t wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, model_path, -1, wpath, (int)(sizeof wpath / sizeof *wpath));
    if (ort_bad(ort, ort->CreateSession(d->env, wpath, d->opts, &d->sess), "CreateSession")) goto fail;
#else
    if (ort_bad(ort, ort->CreateSession(d->env, model_path, d->opts, &d->sess), "CreateSession")) goto fail;
#endif
    if (ort_bad(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &d->mem), "CpuMemoryInfo")) goto fail;

    if (ort_bad(ort, ort->GetAllocatorWithDefaultOptions(&alloc), "GetAllocator")) goto fail;
    if (ort_bad(ort, ort->SessionGetInputName(d->sess, 0, alloc, &d->in_name), "GetInputName")) goto fail;
    if (ort_bad(ort, ort->SessionGetOutputName(d->sess, 0, alloc, &d->out_name), "GetOutputName")) goto fail;

    d->in_buf = malloc((size_t)3 * d->S * d->S * sizeof(float));
    if (!d->in_buf) goto fail;

    snprintf(d->status, sizeof d->status, "%s:%s (%s)", bsdr_depth_tier_name(tier), mi->name, d->ep);
    BSDR_INFO("bsdr.depth", "loaded %s [%dx%d] in=%s out=%s", d->status, d->S, d->S, d->in_name, d->out_name);
    return d;
fail:
    bsdr_depth_close(d);
    return NULL;
}

int bsdr_depth_infer(bsdr_depth *d, const uint8_t *gray, int w, int h, float *out) {
    if (!d || !gray || !out || w < 1 || h < 1) return -1;
    const OrtApi *ort = d->ort;
    int S = d->S;

    /* ---- preprocess: resize gray -> SxS, replicate to 3 channels, normalize, NCHW ---- */
    for (int y = 0; y < S; y++) {
        float sy = (S > 1) ? (float)y * (h - 1) / (S - 1) : 0.f;
        for (int x = 0; x < S; x++) {
            float sx = (S > 1) ? (float)x * (w - 1) / (S - 1) : 0.f;
            float g = sample_u8(gray, w, h, sx, sy) / 255.0f;
            for (int c = 0; c < 3; c++)   /* gray->3ch, [0,1], per-model (v-mean)/std, NCHW */
                d->in_buf[(size_t)c * S * S + (size_t)y * S + x] = (g - d->mi->mean[c]) / d->mi->std[c];
        }
    }

    int64_t shape[4] = { 1, 3, S, S };
    OrtValue *in_val = NULL, *out_val = NULL;
    int rc = -1;
    if (ort_bad(ort, ort->CreateTensorWithDataAsOrtValue(d->mem, d->in_buf,
                    (size_t)3 * S * S * sizeof(float), shape, 4,
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val), "CreateTensor")) return -1;

    const char *ins[1] = { d->in_name }, *outs[1] = { d->out_name };
    if (ort_bad(ort, ort->Run(d->sess, NULL, ins, (const OrtValue *const *)&in_val, 1, outs, 1, &out_val), "Run"))
        goto done;

    /* ---- output: single-channel depth; find its H,W from the last two dims ---- */
    float *od = NULL;
    if (ort_bad(ort, ort->GetTensorMutableData(out_val, (void **)&od), "GetTensorData")) goto done;
    OrtTensorTypeAndShapeInfo *info = NULL;
    if (ort_bad(ort, ort->GetTensorTypeAndShape(out_val, &info), "GetShape")) goto done;
    size_t nd = 0; ort_drop(ort, ort->GetDimensionsCount(info, &nd));
    int64_t dims[8] = {0};
    if (nd > 8) nd = 8;
    ort_drop(ort, ort->GetDimensions(info, dims, nd));
    ort->ReleaseTensorTypeAndShapeInfo(info);
    int oh = nd >= 2 ? (int)dims[nd - 2] : S, ow = nd >= 1 ? (int)dims[nd - 1] : S;
    if (oh < 1 || ow < 1) { oh = S; ow = S; }

    /* min-max normalize to 0..1 (larger = nearer for both MiDaS and DA-V2) */
    size_t n = (size_t)oh * ow;
    float lo = od[0], hi = od[0];
    for (size_t i = 1; i < n; i++) { if (od[i] < lo) lo = od[i]; if (od[i] > hi) hi = od[i]; }
    float inv = hi > lo ? 1.0f / (hi - lo) : 0.0f;

    /* resize normalized depth (ow x oh) -> caller's w x h */
    for (int y = 0; y < h; y++) {
        float syf = (h > 1) ? (float)y * (oh - 1) / (h - 1) : 0.f;
        for (int x = 0; x < w; x++) {
            float sxf = (w > 1) ? (float)x * (ow - 1) / (w - 1) : 0.f;
            float v = sample_f(od, ow, oh, sxf, syf);
            out[(size_t)y * w + x] = inv ? (v - lo) * inv : 0.5f;
        }
    }
    rc = 0;
done:
    if (out_val) ort->ReleaseValue(out_val);
    ort->ReleaseValue(in_val);
    return rc;
}

void bsdr_depth_close(bsdr_depth *d) {
    if (!d) return;
    const OrtApi *ort = d->ort;
    if (ort) {
        OrtAllocator *alloc = NULL;
        ort_drop(ort, ort->GetAllocatorWithDefaultOptions(&alloc));
        if (alloc) { if (d->in_name) alloc->Free(alloc, d->in_name); if (d->out_name) alloc->Free(alloc, d->out_name); }
        if (d->mem)  ort->ReleaseMemoryInfo(d->mem);
        if (d->sess) ort->ReleaseSession(d->sess);
        if (d->opts) ort->ReleaseSessionOptions(d->opts);
        if (d->env)  ort->ReleaseEnv(d->env);
    }
    free(d->in_buf);
    free(d);
}

const char *bsdr_depth_status(bsdr_depth *d) { return d ? d->status : "unavailable"; }

#else /* !BSDR_HAVE_ONNX — stubs so callers link and fall back to co-process/heuristic */

int  bsdr_depth_available(void) { return 0; }
bsdr_depth *bsdr_depth_open(bsdr_depth_tier tier) { (void)tier; return NULL; }
int  bsdr_depth_infer(bsdr_depth *d, const uint8_t *g, int w, int h, float *o) { (void)d;(void)g;(void)w;(void)h;(void)o; return -1; }
void bsdr_depth_close(bsdr_depth *d) { (void)d; }
const char *bsdr_depth_status(bsdr_depth *d) { (void)d; return "unavailable"; }

#endif /* BSDR_HAVE_ONNX */
