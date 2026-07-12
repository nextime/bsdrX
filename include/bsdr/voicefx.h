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
    int gender;   /* -100..100 PITCH+formant character (granular shift): <0 deeper, >0 higher, 0 = off */
    int formant;  /* -100..100 tone/vocal-tract brightness: <0 darker/bigger, >0 brighter/smaller, 0 = off */
    int volume;   /* -100..100 output gain: 0 = unity (+/-100 ~= +/-12 dB) */
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

/* Select the AI (RVC voice-conversion) tier, which sits behind this same interface. When `on`, the
 * mic is converted to the target `voice` (an RVC .onnx) using the `content`/`rmvpe` base models on the
 * given `tier` (1=cpu 2=small-gpu 3=big-gpu), pitch-shifted by `key_semitones`; the DSP knobs are
 * bypassed except `volume` (applied as post-gain). The engine reloads only when the config actually
 * changes (safe to call every reconcile). `on`=0 (or an unavailable/absent model) reverts to the DSP
 * tier. No-op to a passthrough when the ONNX build isn't present. `voice_sr` = the voice model's
 * native rate (0 = 40000). */
void bsdr_voicefx_set_ai(bsdr_voicefx *v, int on, int tier, const char *content, const char *rmvpe,
                         const char *voice, int voice_sr, float key_semitones);
/* 1 if the AI tier is selected AND its engine is loaded and running. */
int bsdr_voicefx_ai_active(const bsdr_voicefx *v);

/* True if any effect is active (so the caller can skip processing when everything is 0). */
int bsdr_voicefx_active(const bsdr_voicefx *v);

/* Process `frames` mono samples in place. A passthrough when nothing is enabled. */
void bsdr_voicefx_process(bsdr_voicefx *v, int16_t *pcm, int frames);

void bsdr_voicefx_free(bsdr_voicefx *v);

#endif /* BSDR_VOICEFX_H */
