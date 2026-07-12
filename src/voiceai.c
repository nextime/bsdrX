/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* voiceai.c — RVC voice conversion via ONNX Runtime. See voiceai.h. */
#include "bsdr/voiceai.h"
#include "bsdr/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int bsdr_voiceai_available(void) {
#ifdef BSDR_HAVE_ONNX
    return 1;
#else
    return 0;
#endif
}

#ifdef BSDR_HAVE_ONNX
#include "onnxruntime_c_api.h"
#ifdef _WIN32
#  include <windows.h>
#endif
#ifdef __ANDROID__
#  include <dlfcn.h>
#endif
#if defined(_WIN32) && defined(BSDR_ONNX_DML)
#  include "dml_provider_factory.h"
#endif
#if defined(BSDR_ONNX_CUDA)
#  include "cuda_provider_factory.h"
#endif

/* ---- tuning (all at the caller's io rate unless noted) --------------------------------------- */
#define HUBERT_SR   16000            /* ContentVec / RMVPE input rate */
#define F0_HOP16    160              /* 16k / 100Hz -> f0 frame every 10ms */
#define F0_MIN      50.0f
#define F0_MAX      1100.0f
#define WIN_MS      360              /* processing window (latency ~= this) */
#define CROSS_MS    64               /* crossfade overlap */
#define RING_MS     4000             /* internal FIFO capacity */

/* ---- one ORT model session ------------------------------------------------------------------- */
typedef struct {
    OrtSession *sess;
    int   n_in, n_out;
    char *in_name[8];
    char *out_name[4];
    ONNXTensorElementDataType in_type[8];
    int   in_rank[8];
    int64_t in_dim[8][6];
} ort_model;

struct bsdr_voiceai {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtMemoryInfo *mem;
    OrtAllocator *alloc;
    ort_model content, rmvpe, gen;
    int have_rmvpe;
    int io_sr, voice_sr;
    int tier;
    int ready;
    char status[128];
    const char *ep;

    /* streaming state (all float, io_sr) */
    float *in_accum;  int in_len, in_cap;      /* pending input */
    float *out_fifo;  int out_head, out_tail, out_cap;  /* converted output ring */
    float *prev_tail; int cross;                /* last CROSS samples of the previous window */
    int win, step;
    float key;
};

static int ort_fail(const OrtApi *ort, OrtStatus *st, const char *what) {
    if (!st) return 0;
    BSDR_WARN("bsdr.voiceai", "%s: %s", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return 1;
}
static void ort_drop(const OrtApi *ort, OrtStatus *st) { if (st) ort->ReleaseStatus(st); }

/* Attach the best EP for the tier (mirrors depth_onnx.c). GPU tiers try platform accel then CUDA. */
static void select_ep(bsdr_voiceai *v, OrtSessionOptions *o) {
    const OrtApi *ort = v->ort;
    v->ep = "cpu";
    int want_gpu = (v->tier >= 2);
    if (want_gpu) {
#if defined(__ANDROID__)
        typedef OrtStatus *(*nnapi_fn)(OrtSessionOptions *, uint32_t);
        nnapi_fn nn = (nnapi_fn)dlsym(RTLD_DEFAULT, "OrtSessionOptionsAppendExecutionProvider_Nnapi");
        if (nn) { OrtStatus *st = nn(o, 0); if (!st) { v->ep = "nnapi"; return; } ort->ReleaseStatus(st); }
#endif
        const char *gpu_ep = NULL;
#if defined(__APPLE__)
        gpu_ep = "CoreML";
#endif
        if (gpu_ep) { OrtStatus *st = ort->SessionOptionsAppendExecutionProvider(o, gpu_ep, NULL, NULL, 0);
                      if (!st) { v->ep = gpu_ep; return; } ort->ReleaseStatus(st); }
#if defined(_WIN32) && defined(BSDR_ONNX_DML)
        OrtStatus *dst = OrtSessionOptionsAppendExecutionProvider_DML(o, 0);
        if (!dst) { v->ep = "dml"; return; } ort->ReleaseStatus(dst);
#endif
#if defined(BSDR_ONNX_CUDA)
        OrtCUDAProviderOptions cuda; memset(&cuda, 0, sizeof cuda);
        OrtStatus *cst = ort->SessionOptionsAppendExecutionProvider_CUDA(o, &cuda);
        if (!cst) { v->ep = "cuda"; return; } ort->ReleaseStatus(cst);
#endif
    }
    OrtStatus *st = ort->SessionOptionsAppendExecutionProvider(o, "XNNPACK", NULL, NULL, 0);
    if (!st) v->ep = "xnnpack"; else ort->ReleaseStatus(st);
}

static int model_open(bsdr_voiceai *v, ort_model *m, const char *path) {
    const OrtApi *ort = v->ort;
    memset(m, 0, sizeof *m);
#ifdef _WIN32
    wchar_t wp[1024]; MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024);
    if (ort_fail(ort, ort->CreateSession(v->env, wp, v->opts, &m->sess), "CreateSession")) return -1;
#else
    if (ort_fail(ort, ort->CreateSession(v->env, path, v->opts, &m->sess), "CreateSession")) return -1;
#endif
    size_t ni = 0, no = 0;
    ort_drop(ort, ort->SessionGetInputCount(m->sess, &ni));
    ort_drop(ort, ort->SessionGetOutputCount(m->sess, &no));
    m->n_in = ni > 8 ? 8 : (int)ni; m->n_out = no > 4 ? 4 : (int)no;
    for (int i = 0; i < m->n_in; i++) {
        ort_drop(ort, ort->SessionGetInputName(m->sess, i, v->alloc, &m->in_name[i]));
        OrtTypeInfo *ti = NULL;
        if (!ort_fail(ort, ort->SessionGetInputTypeInfo(m->sess, i, &ti), "InTypeInfo") && ti) {
            const OrtTensorTypeAndShapeInfo *si = NULL;
            ort_drop(ort, ort->CastTypeInfoToTensorInfo(ti, &si));
            if (si) {
                ort_drop(ort, ort->GetTensorElementType(si, &m->in_type[i]));
                size_t nd = 0; ort_drop(ort, ort->GetDimensionsCount(si, &nd));
                m->in_rank[i] = nd > 6 ? 6 : (int)nd;
                ort_drop(ort, ort->GetDimensions(si, m->in_dim[i], m->in_rank[i]));
            }
            ort->ReleaseTypeInfo(ti);
        }
    }
    for (int i = 0; i < m->n_out; i++)
        ort_drop(ort, ort->SessionGetOutputName(m->sess, i, v->alloc, &m->out_name[i]));
    return 0;
}
static void model_close(bsdr_voiceai *v, ort_model *m) {
    const OrtApi *ort = v->ort;
    if (v->alloc) {
        for (int i = 0; i < m->n_in; i++)  if (m->in_name[i])  v->alloc->Free(v->alloc, m->in_name[i]);
        for (int i = 0; i < m->n_out; i++) if (m->out_name[i]) v->alloc->Free(v->alloc, m->out_name[i]);
    }
    if (m->sess) ort->ReleaseSession(m->sess);
    memset(m, 0, sizeof *m);
}

/* ---- signal helpers -------------------------------------------------------------------------- */
/* Linear resample mono `n` samples at sr_in -> `*out_n` at sr_out (malloc'd). */
static float *resample(const float *in, int n, int sr_in, int sr_out, int *out_n) {
    if (sr_in == sr_out) { float *o = malloc((size_t)n * sizeof(float)); if (o) memcpy(o, in, (size_t)n * sizeof(float)); *out_n = n; return o; }
    int m = (int)((int64_t)n * sr_out / sr_in);
    if (m < 1) m = 1;
    float *o = malloc((size_t)m * sizeof(float)); if (!o) { *out_n = 0; return NULL; }
    double step = (double)(n - 1) / (double)(m > 1 ? m - 1 : 1);
    for (int i = 0; i < m; i++) {
        double x = i * step; int x0 = (int)x; double fr = x - x0;
        int x1 = x0 + 1 < n ? x0 + 1 : x0;
        o[i] = (float)(in[x0] * (1.0 - fr) + in[x1] * fr);
    }
    *out_n = m; return o;
}

/* Autocorrelation pitch on a 16k window -> f0[nframes] (0 = unvoiced). CPU F0 (RMVPE-free). */
static void dsp_f0(const float *a16, int n16, float *f0, int nf) {
    int minlag = (int)(HUBERT_SR / F0_MAX), maxlag = (int)(HUBERT_SR / F0_MIN);
    int wlen = maxlag * 2;
    for (int t = 0; t < nf; t++) {
        int c = t * F0_HOP16;
        int s = c - wlen / 2; if (s < 0) s = 0;
        int e = s + wlen; if (e > n16) { e = n16; s = e - wlen; if (s < 0) s = 0; }
        int len = e - s; if (len < maxlag + 1) { f0[t] = 0; continue; }
        const float *w = a16 + s;
        double e0 = 1e-9; for (int i = 0; i < len; i++) e0 += (double)w[i] * w[i];
        double best = 0; int bl = 0;
        for (int lag = minlag; lag <= maxlag && lag < len; lag++) {
            double acc = 0; for (int i = 0; i + lag < len; i++) acc += (double)w[i] * w[i + lag];
            double norm = acc / e0;
            if (norm > best) { best = norm; bl = lag; }
        }
        f0[t] = (best > 0.30 && bl > 0) ? (float)HUBERT_SR / bl : 0.0f;   /* voicing threshold */
    }
}

/* f0 (Hz) -> coarse pitch (1..255, RVC mel-bin) + fine pitchf (Hz), applying the key shift. */
static void f0_to_rvc(const float *f0, int nf, float key, int64_t *coarse, float *pitchf) {
    float mel_min = 1127.0f * logf(1.0f + F0_MIN / 700.0f);
    float mel_max = 1127.0f * logf(1.0f + F0_MAX / 700.0f);
    float scale = powf(2.0f, key / 12.0f);
    for (int i = 0; i < nf; i++) {
        float f = f0[i] * scale;
        pitchf[i] = f;
        if (f <= 0) { coarse[i] = 0; continue; }
        float mel = 1127.0f * logf(1.0f + f / 700.0f);
        float b = (mel - mel_min) * 254.0f / (mel_max - mel_min) + 1.0f;
        int bi = (int)(b + 0.5f);
        coarse[i] = bi < 1 ? 1 : (bi > 255 ? 255 : bi);
    }
}

/* ---- one RVC forward pass over a mono io-rate window -> converted mono io-rate (malloc'd) ----- */
static float *rvc_infer(bsdr_voiceai *v, const float *win, int wlen, int *out_len) {
    const OrtApi *ort = v->ort;
    *out_len = 0;
    /* 1) -> 16k for the content encoder + F0 */
    int n16 = 0; float *a16 = resample(win, wlen, v->io_sr, HUBERT_SR, &n16);
    if (!a16 || n16 < F0_HOP16 * 4) { free(a16); return NULL; }

    /* 2) ContentVec: audio[1,N] (or [1,1,N]) -> feats[1,Tc,C] */
    int64_t cs2[2] = { 1, n16 }, cs3[3] = { 1, 1, n16 };
    int use3 = (v->content.n_in > 0 && v->content.in_rank[0] >= 3);
    OrtValue *cin = NULL;
    if (ort_fail(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, a16, (size_t)n16 * sizeof(float),
                    use3 ? cs3 : cs2, use3 ? 3 : 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &cin), "content in")) { free(a16); return NULL; }
    const char *cin_names[1] = { v->content.in_name[0] };
    const char *cout_names[1] = { v->content.out_name[0] };
    OrtValue *cout = NULL;
    if (ort_fail(ort, ort->Run(v->content.sess, NULL, cin_names, (const OrtValue *const *)&cin, 1,
                    cout_names, 1, &cout), "content run")) { ort->ReleaseValue(cin); free(a16); return NULL; }
    /* feats shape [1,Tc,C] */
    OrtTensorTypeAndShapeInfo *fi = NULL; int64_t fdim[4] = {1,1,1,1}; size_t fnd = 0;
    ort_drop(ort, ort->GetTensorTypeAndShape(cout, &fi));
    if (fi) { ort_drop(ort, ort->GetDimensionsCount(fi, &fnd)); if (fnd > 4) fnd = 4; ort_drop(ort, ort->GetDimensions(fi, fdim, fnd)); ort->ReleaseTensorTypeAndShapeInfo(fi); }
    int Tc = (int)fdim[fnd >= 2 ? fnd - 2 : 0];
    int C  = (int)fdim[fnd >= 1 ? fnd - 1 : 0];
    float *feat = NULL; ort_drop(ort, ort->GetTensorMutableData(cout, (void **)&feat));
    if (!feat || Tc < 2 || C < 16) { ort->ReleaseValue(cin); ort->ReleaseValue(cout); free(a16); return NULL; }

    /* 3) interpolate feats x2 along time (RVC upsamples content to the f0 rate) -> T = 2*Tc */
    int T = Tc * 2;
    float *phone = malloc((size_t)T * C * sizeof(float));
    if (!phone) { ort->ReleaseValue(cin); ort->ReleaseValue(cout); free(a16); return NULL; }
    for (int t = 0; t < T; t++) {
        float sx = (float)t * (Tc - 1) / (T > 1 ? T - 1 : 1);
        int x0 = (int)sx; float fr = sx - x0; int x1 = x0 + 1 < Tc ? x0 + 1 : x0;
        for (int c = 0; c < C; c++)
            phone[(size_t)t * C + c] = feat[(size_t)x0 * C + c] * (1 - fr) + feat[(size_t)x1 * C + c] * fr;
    }
    ort->ReleaseValue(cin); ort->ReleaseValue(cout);

    /* 4) F0 at length T (10ms frames). RMVPE on GPU tiers if loaded, else DSP autocorrelation. */
    float *f0 = malloc((size_t)T * sizeof(float));
    int64_t *coarse = malloc((size_t)T * sizeof(int64_t));
    float *pitchf = malloc((size_t)T * sizeof(float));
    if (!f0 || !coarse || !pitchf) { free(phone); free(f0); free(coarse); free(pitchf); free(a16); return NULL; }
    dsp_f0(a16, n16, f0, T);          /* robust default; RMVPE path can refine later */
    f0_to_rvc(f0, T, v->key, coarse, pitchf);
    free(a16);

    /* 5) RVC generator — build inputs by name (v1/v2 tolerant) */
    OrtValue *gin[8]; memset(gin, 0, sizeof gin);
    const char *gin_names[8];
    int64_t sh_phone[3] = { 1, T, C };
    int64_t sh_len[1] = { 1 }, len_val[1] = { T };
    int64_t sh_pt[2] = { 1, T };
    int64_t sid_val[1] = { 0 };
    float *rnd = NULL;
    int ng = v->gen.n_in;
    for (int i = 0; i < ng; i++) {
        const char *nm = v->gen.in_name[i]; gin_names[i] = nm;
        int is_i64 = (v->gen.in_type[i] == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        if (strstr(nm, "length") || (!strcmp(nm, "p_len"))) {
            ort_drop(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, len_val, sizeof len_val, sh_len, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &gin[i]));
        } else if (strstr(nm, "pitchf") || strstr(nm, "nsff0") || (strstr(nm, "pitch") && !is_i64)) {
            ort_drop(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, pitchf, (size_t)T * sizeof(float), sh_pt, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &gin[i]));
        } else if (strstr(nm, "pitch") || (is_i64 && strstr(nm, "f0"))) {
            ort_drop(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, coarse, (size_t)T * sizeof(int64_t), sh_pt, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &gin[i]));
        } else if (!strcmp(nm, "ds") || !strcmp(nm, "sid") || strstr(nm, "speaker")) {
            ort_drop(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, sid_val, sizeof sid_val, sh_len, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &gin[i]));
        } else if (strstr(nm, "rnd") || strstr(nm, "rand") || strstr(nm, "noise")) {
            int ch = v->gen.in_rank[i] >= 3 ? (int)v->gen.in_dim[i][1] : 192; if (ch < 1) ch = 192;
            int64_t sh_r[3] = { 1, ch, T }; size_t rn = (size_t)ch * T;
            rnd = malloc(rn * sizeof(float));
            for (size_t k = 0; k < rn; k++) {   /* ~N(0,1) via CLT of 4 uniforms (no rand seed dep) */
                float s = 0; for (int q = 0; q < 4; q++) s += (float)((k * 1103515245u + q * 12345u) & 0xffff) / 65535.0f;
                rnd[k] = (s - 2.0f) * 0.9f;
            }
            ort_drop(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, rnd, rn * sizeof(float), sh_r, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &gin[i]));
        } else {   /* default: the content/phone features */
            ort_drop(ort, ort->CreateTensorWithDataAsOrtValue(v->mem, phone, (size_t)T * C * sizeof(float), sh_phone, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &gin[i]));
        }
    }
    OrtValue *gout = NULL;
    const char *gout_names[1] = { v->gen.out_name[0] };
    float *conv = NULL;
    int convn = 0;
    if (!ort_fail(ort, ort->Run(v->gen.sess, NULL, gin_names, (const OrtValue *const *)gin, ng, gout_names, 1, &gout), "gen run")) {
        OrtTensorTypeAndShapeInfo *oi = NULL; int64_t od[4] = {1,1,1,1}; size_t ond = 0;
        ort_drop(ort, ort->GetTensorTypeAndShape(gout, &oi));
        if (oi) { ort_drop(ort, ort->GetDimensionsCount(oi, &ond)); if (ond > 4) ond = 4; ort_drop(ort, ort->GetDimensions(oi, od, ond)); ort->ReleaseTensorTypeAndShapeInfo(oi); }
        int nout = (int)od[ond >= 1 ? ond - 1 : 0]; if (nout < 1) nout = 1;
        float *raw = NULL; ort_drop(ort, ort->GetTensorMutableData(gout, (void **)&raw));
        if (raw) conv = resample(raw, nout, v->voice_sr, v->io_sr, &convn);   /* voice SR -> io SR */
    }
    for (int i = 0; i < ng; i++) if (gin[i]) ort->ReleaseValue(gin[i]);
    if (gout) ort->ReleaseValue(gout);
    free(phone); free(f0); free(coarse); free(pitchf); free(rnd);
    *out_len = convn;
    return conv;
}

/* ---- lifecycle ------------------------------------------------------------------------------- */
bsdr_voiceai *bsdr_voiceai_open(int tier, const char *content_onnx, const char *rmvpe_onnx,
                                const char *voice_onnx, int io_sample_rate, int voice_sr) {
    if (!content_onnx || !content_onnx[0] || !voice_onnx || !voice_onnx[0]) return NULL;
    bsdr_voiceai *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->tier = tier < 1 ? 1 : tier;
    v->io_sr = io_sample_rate > 0 ? io_sample_rate : 48000;
    v->voice_sr = voice_sr > 0 ? voice_sr : 40000;
    v->key = 0;
    v->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!v->ort) { free(v); return NULL; }
    const OrtApi *ort = v->ort;
    if (ort_fail(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "bsdr.voiceai", &v->env), "CreateEnv")) goto fail;
    if (ort_fail(ort, ort->CreateSessionOptions(&v->opts), "CreateSessionOptions")) goto fail;
    ort_drop(ort, ort->SetSessionGraphOptimizationLevel(v->opts, ORT_ENABLE_ALL));
    select_ep(v, v->opts);
    if (ort_fail(ort, ort->GetAllocatorWithDefaultOptions(&v->alloc), "GetAllocator")) goto fail;
    if (ort_fail(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &v->mem), "CpuMemoryInfo")) goto fail;

    if (model_open(v, &v->content, content_onnx) != 0) goto fail;
    if (model_open(v, &v->gen, voice_onnx) != 0) goto fail;
    if (rmvpe_onnx && rmvpe_onnx[0] && model_open(v, &v->rmvpe, rmvpe_onnx) == 0) v->have_rmvpe = 1;

    v->win  = v->io_sr * WIN_MS / 1000;
    v->cross = v->io_sr * CROSS_MS / 1000;
    v->step = v->win - v->cross;
    v->in_cap = v->win + v->io_sr;            /* window + slack */
    v->out_cap = v->io_sr * RING_MS / 1000;
    v->in_accum  = malloc((size_t)v->in_cap * sizeof(float));
    v->out_fifo  = malloc((size_t)v->out_cap * sizeof(float));
    v->prev_tail = calloc((size_t)v->cross, sizeof(float));
    if (!v->in_accum || !v->out_fifo || !v->prev_tail) goto fail;

    snprintf(v->status, sizeof v->status, "tier%d:%s rvc%s win%dms", v->tier, v->ep,
             v->have_rmvpe ? "+rmvpe" : "", WIN_MS);
    v->ready = 1;
    BSDR_INFO("bsdr.voiceai", "RVC engine up (%s) content=%dch gen-inputs=%d", v->status, v->content.n_out, v->gen.n_in);
    return v;
fail:
    bsdr_voiceai_close(v);
    return NULL;
}

int bsdr_voiceai_ready(const bsdr_voiceai *v) { return v && v->ready; }
const char *bsdr_voiceai_status(const bsdr_voiceai *v) { return v ? v->status : "unavailable"; }

static void fifo_push(bsdr_voiceai *v, const float *s, int n) {
    for (int i = 0; i < n; i++) {
        int nt = (v->out_tail + 1) % v->out_cap;
        if (nt == v->out_head) v->out_head = (v->out_head + 1) % v->out_cap;   /* overwrite oldest on overflow */
        v->out_fifo[v->out_tail] = s[i]; v->out_tail = nt;
    }
}
static int fifo_avail(bsdr_voiceai *v) {
    return (v->out_tail - v->out_head + v->out_cap) % v->out_cap;
}
static int fifo_pop(bsdr_voiceai *v, float *out, int n) {
    int got = 0;
    while (got < n && v->out_head != v->out_tail) { out[got++] = v->out_fifo[v->out_head]; v->out_head = (v->out_head + 1) % v->out_cap; }
    return got;
}

void bsdr_voiceai_process(bsdr_voiceai *v, const int16_t *in, int16_t *out, int frames, float key_semitones) {
    if (!v || !v->ready) { if (in != (const int16_t *)out) memcpy(out, in, (size_t)frames * sizeof(int16_t)); return; }
    v->key = key_semitones;
    /* accumulate input as float */
    for (int i = 0; i < frames; i++) {
        if (v->in_len < v->in_cap) v->in_accum[v->in_len++] = in[i] / 32768.0f;
    }
    /* process full windows, advancing by step, crossfading the overlap */
    while (v->in_len >= v->win) {
        int cl = 0; float *conv = rvc_infer(v, v->in_accum, v->win, &cl);
        if (conv && cl >= v->win) {
            /* crossfade first `cross` with the previous window's tail */
            for (int i = 0; i < v->cross; i++) {
                float t = (float)i / (float)v->cross;
                conv[i] = v->prev_tail[i] * (1 - t) + conv[i] * t;
            }
            fifo_push(v, conv, v->step);                         /* emit step samples */
            memcpy(v->prev_tail, conv + v->step, (size_t)v->cross * sizeof(float));
        } else {
            /* inference failed: emit passthrough for this step so audio keeps flowing */
            fifo_push(v, v->in_accum, v->step);
            memset(v->prev_tail, 0, (size_t)v->cross * sizeof(float));
        }
        free(conv);
        memmove(v->in_accum, v->in_accum + v->step, (size_t)(v->in_len - v->step) * sizeof(float));
        v->in_len -= v->step;
    }
    /* drain output; passthrough during warm-up (fifo not yet primed) */
    if (fifo_avail(v) >= frames) {
        float tmp[8192];
        int done = 0;
        while (done < frames) {
            int chunk = frames - done; if (chunk > 8192) chunk = 8192;
            int g = fifo_pop(v, tmp, chunk);
            for (int i = 0; i < g; i++) {
                float s = tmp[i] * 32768.0f;
                out[done + i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
            }
            done += g; if (g < chunk) break;
        }
        for (; done < frames; done++) out[done] = 0;
    } else {
        if (in != (const int16_t *)out) memcpy(out, in, (size_t)frames * sizeof(int16_t));
    }
}

void bsdr_voiceai_close(bsdr_voiceai *v) {
    if (!v) return;
    const OrtApi *ort = v->ort;
    if (ort) {
        model_close(v, &v->content); model_close(v, &v->gen);
        if (v->have_rmvpe) model_close(v, &v->rmvpe);
        if (v->mem)  ort->ReleaseMemoryInfo(v->mem);
        if (v->opts) ort->ReleaseSessionOptions(v->opts);
        if (v->env)  ort->ReleaseEnv(v->env);
    }
    free(v->in_accum); free(v->out_fifo); free(v->prev_tail);
    free(v);
}

#else  /* !BSDR_HAVE_ONNX — stubs */

bsdr_voiceai *bsdr_voiceai_open(int tier, const char *c, const char *r, const char *voi, int io, int vsr) {
    (void)tier;(void)c;(void)r;(void)voi;(void)io;(void)vsr; return NULL;
}
int  bsdr_voiceai_ready(const bsdr_voiceai *v) { (void)v; return 0; }
void bsdr_voiceai_process(bsdr_voiceai *v, const int16_t *in, int16_t *out, int frames, float key) {
    (void)v;(void)key; if (in != (const int16_t *)out) memcpy(out, in, (size_t)frames * sizeof(int16_t));
}
const char *bsdr_voiceai_status(const bsdr_voiceai *v) { (void)v; return "unavailable"; }
void bsdr_voiceai_close(bsdr_voiceai *v) { (void)v; }

#endif /* BSDR_HAVE_ONNX */
