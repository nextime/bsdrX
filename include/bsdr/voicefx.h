/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* voicefx.h — realtime voice changer (gender shift) applied to the owner-mic PCM before it goes to
 * the cloud. This is the no-model DSP tier: a streaming delay-line (granular) pitch shifter that
 * scales BOTH pitch and formants by one factor — which is exactly the dominant cue for a gender
 * change. It's O(1) per sample, cross-platform (no FFT/ffmpeg), and low latency. A higher-quality
 * model-based tier (RVC voice conversion) is planned to sit behind the same interface later.
 *
 * Operates on mono int16 PCM in place (same length in/out), so it drops straight into the PCM-sink
 * chain (micsniff / cloud room audio). */
#ifndef BSDR_VOICEFX_H
#define BSDR_VOICEFX_H

#include <stdint.h>

typedef struct bsdr_voicefx bsdr_voicefx;

/* All the voice-changer knobs (0 on every field = passthrough). */
typedef struct {
    int gender;   /* -100..100 pitch+formant: <0 deeper/male, >0 higher/female, 0 = off */
    int robot;    /* 0..100 ring-modulation mix — a robotic/vocoder timbre */
    int echo;     /* 0..100 echo/reverb mix — adds a trailing echo */
    int whisper;  /* 0..100 breathiness — blends in noise, removes voicing */
} bsdr_voicefx_params;

/* Create a voice changer for `sample_rate` Hz mono. Returns NULL on OOM. */
bsdr_voicefx *bsdr_voicefx_new(int sample_rate);

/* Set all effects at once. Safe to call between process() calls. */
void bsdr_voicefx_set_params(bsdr_voicefx *v, const bsdr_voicefx_params *p);

/* Pitch/formant scale factor: 1.0 = bypass, >1 raises, <1 lowers. Clamped to [0.5, 2.0]. */
void bsdr_voicefx_set_shift(bsdr_voicefx *v, float shift);

/* Convenience: gender knob [-100,100] -> shift factor (leaves the other effects unchanged). */
void bsdr_voicefx_set_gender(bsdr_voicefx *v, int gender);

/* True if any effect is active (so the caller can skip processing when everything is 0). */
int bsdr_voicefx_active(const bsdr_voicefx *v);

/* Process `frames` mono samples in place. A passthrough when nothing is enabled. */
void bsdr_voicefx_process(bsdr_voicefx *v, int16_t *pcm, int frames);

void bsdr_voicefx_free(bsdr_voicefx *v);

#endif /* BSDR_VOICEFX_H */
