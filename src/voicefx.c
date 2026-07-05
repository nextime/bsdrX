/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* voicefx.c — streaming delay-line (granular) pitch shifter. See voicefx.h.
 *
 * Two read taps sweep a delay line at rate (shift-1) and are cross-faded with a triangular window so
 * the wrap discontinuity of one tap lands in the other tap's fade-out. Reading the delay line faster
 * or slower than it's written scales the playback rate — i.e. pitch AND formants — by `shift`, with
 * the two-tap crossfade keeping the output continuous and the duration unchanged. No FFT, O(1)/sample,
 * so it runs identically on every platform (incl. Android, which has no ffmpeg). */
#include "bsdr/voicefx.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

struct bsdr_voicefx {
    int    sr;
    float  shift;      /* target pitch/formant scale */
    float *buf;        /* pitch-shifter delay line (mono float) */
    int    len;        /* delay-line length in samples */
    int    wr;         /* write index */
    float  phase;      /* read-delay sweep position, [0, len) */
    /* extra effects */
    float  robot;      /* 0..1 ring-mod mix */
    float  echo;       /* 0..1 echo mix */
    float  whisper;    /* 0..1 breathiness */
    float  carrier;    /* ring-mod carrier phase (radians) */
    float *edl;        /* echo delay line */
    int    elen, ewr;  /* echo line length + write index */
    uint32_t rng;      /* xorshift noise for whisper */
};

bsdr_voicefx *bsdr_voicefx_new(int sample_rate) {
    if (sample_rate <= 0) sample_rate = 48000;
    bsdr_voicefx *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->sr = sample_rate;
    v->shift = 1.0f;
    /* ~50 ms window: long enough that the sweep period is sub-audible, short enough for low latency. */
    v->len = sample_rate / 20;
    if (v->len < 256) v->len = 256;
    v->buf = calloc((size_t)v->len, sizeof(float));
    if (!v->buf) { free(v); return NULL; }
    v->phase = 0.0f;
    v->elen = sample_rate / 3;                 /* ~330 ms echo line */
    if (v->elen < 1024) v->elen = 1024;
    v->edl = calloc((size_t)v->elen, sizeof(float));
    if (!v->edl) { free(v->buf); free(v); return NULL; }
    v->rng = 0x9e3779b9u;
    return v;
}

void bsdr_voicefx_set_params(bsdr_voicefx *v, const bsdr_voicefx_params *p) {
    if (!v || !p) return;
    bsdr_voicefx_set_gender(v, p->gender);
    int r = p->robot < 0 ? 0 : p->robot > 100 ? 100 : p->robot;
    int e = p->echo   < 0 ? 0 : p->echo   > 100 ? 100 : p->echo;
    int w = p->whisper< 0 ? 0 : p->whisper> 100 ? 100 : p->whisper;
    v->robot = r / 100.0f;
    v->echo = e / 100.0f;
    v->whisper = w / 100.0f;
}

int bsdr_voicefx_active(const bsdr_voicefx *v) {
    return v && (v->shift != 1.0f || v->robot > 0 || v->echo > 0 || v->whisper > 0);
}

void bsdr_voicefx_set_shift(bsdr_voicefx *v, float shift) {
    if (!v) return;
    if (shift < 0.5f) shift = 0.5f; else if (shift > 2.0f) shift = 2.0f;
    v->shift = shift;
}

void bsdr_voicefx_set_gender(bsdr_voicefx *v, int gender) {
    if (!v) return;
    if (gender < -100) gender = -100; else if (gender > 100) gender = 100;
    /* map [-100,100] -> shift [0.66, 1.5] on a smooth exponential (0 -> 1.0 bypass) */
    float f = (float)gender / 100.0f;         /* [-1, 1] */
    float shift = f >= 0 ? 1.0f + f * 0.5f    /* up to 1.5x (feminize) */
                         : 1.0f + f * 0.34f;  /* down to 0.66x (masculinize) */
    bsdr_voicefx_set_shift(v, shift);
}

/* linear-interpolated read of the delay line at `delay` samples behind the write head */
static inline float tap(const bsdr_voicefx *v, float delay) {
    float rp = (float)v->wr - delay;
    while (rp < 0) rp += v->len;
    while (rp >= v->len) rp -= v->len;
    int i0 = (int)rp;
    int i1 = i0 + 1; if (i1 >= v->len) i1 = 0;
    float frac = rp - (float)i0;
    return v->buf[i0] + (v->buf[i1] - v->buf[i0]) * frac;
}

void bsdr_voicefx_process(bsdr_voicefx *v, int16_t *pcm, int frames) {
    if (!v || !pcm || frames <= 0 || !bsdr_voicefx_active(v)) return;

    const int   L     = v->len;
    const float half  = (float)L * 0.5f;
    /* Playback speed = 1 - rate; we want it to equal `shift`, so the delay sweeps at 1 - shift. */
    const float rate  = 1.0f - v->shift;
    const int   doShift = v->shift != 1.0f;
    const float cinc  = 6.2831853f * 80.0f / (float)v->sr;   /* robot carrier ~80 Hz */

    for (int n = 0; n < frames; n++) {
        float s = (float)pcm[n] * (1.0f / 32768.0f);

        /* 1) pitch/formant shift (two-tap granular delay line) */
        if (doShift) {
            v->buf[v->wr] = s;
            float p1 = v->phase;
            float p2 = v->phase + half; if (p2 >= L) p2 -= L;
            float w1 = 1.0f - (p1 < half ? p1 : (float)L - p1) / half;
            if (w1 < 0) w1 = 0; else if (w1 > 1) w1 = 1;
            s = tap(v, p1) * w1 + tap(v, p2) * (1.0f - w1);
            v->wr++; if (v->wr >= L) v->wr = 0;
            v->phase += rate;
            while (v->phase >= L) v->phase -= L;
            while (v->phase < 0)  v->phase += L;
        }

        /* 2) robot: ring-modulate against a low carrier, mixed with the dry signal */
        if (v->robot > 0.0f) {
            float c = sinf(v->carrier);
            v->carrier += cinc; if (v->carrier > 6.2831853f) v->carrier -= 6.2831853f;
            s = s * (1.0f - v->robot) + (s * c) * v->robot;
        }

        /* 3) whisper: blend amplitude-shaped noise (breathiness) */
        if (v->whisper > 0.0f) {
            v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
            float nz = (float)((int32_t)v->rng) / 2147483648.0f;   /* [-1,1) */
            s = s * (1.0f - v->whisper) + (nz * (s < 0 ? -s : s) * 1.6f) * v->whisper;
        }

        /* 4) echo: a ~330 ms delay tap with feedback */
        if (v->echo > 0.0f) {
            float d = v->edl[v->ewr];
            float out = s + d * v->echo;
            v->edl[v->ewr] = s + d * 0.5f * v->echo;   /* feedback -> decaying repeats */
            v->ewr++; if (v->ewr >= v->elen) v->ewr = 0;
            s = out;
        }

        int val = (int)(s * 32768.0f);
        if (val > 32767) val = 32767; else if (val < -32768) val = -32768;
        pcm[n] = (int16_t)val;
    }
}

void bsdr_voicefx_free(bsdr_voicefx *v) {
    if (!v) return;
    free(v->buf);
    free(v->edl);
    free(v);
}
