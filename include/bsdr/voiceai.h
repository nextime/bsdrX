/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* voiceai.h — AI voice conversion (RVC) via ONNX Runtime, the model-based tier behind bsdr_voicefx.
 *
 * Pipeline (all ONNX, cross-platform via ORT — CPU / XNNPACK / CoreML / NNAPI / DirectML / CUDA):
 *   48k mono in --(resample 16k)--> ContentVec ---> content features -----\
 *                                    \--> F0 (RMVPE on GPU tiers, or a     >--> RVC generator --> 40k
 *                                         built-in DSP pitch tracker CPU) -/         (target voice)  |
 *   48k mono out <----------------------------- crossfade / overlap-add <-- resample 48k <-----------/
 *
 * Streaming: fixed-hop windows with an overlap crossfade, so it presents the SAME same-length,
 * in-place contract bsdr_voicefx already uses — it just adds ~one-window of latency and outputs the
 * (converted) delayed audio. During warm-up / when not ready it passes audio through unchanged.
 *
 * Tier: 1=CPU, 2=small GPU, 3=big GPU. Execution-provider selection mirrors depth_onnx.c's select_ep,
 * so it degrades to CPU everywhere (incl. Android NNAPI). Compiled only with BSDR_HAVE_ONNX; without
 * it the stubs report "unavailable" and bsdr_voicefx keeps using the DSP tier.
 */
#ifndef BSDR_VOICEAI_H
#define BSDR_VOICEAI_H

#include <stdint.h>

typedef struct bsdr_voiceai bsdr_voiceai;

/* 1 if this build has the ONNX engine compiled in (else only the DSP voicefx tier exists). */
int bsdr_voiceai_available(void);

/* Open an RVC engine. `content_onnx` (ContentVec) is required; `rmvpe_onnx` may be NULL/"" (the CPU
 * tier then uses the built-in DSP pitch tracker); `voice_onnx` is the target-voice generator.
 * `io_sample_rate` is the PCM rate the caller feeds/reads (48000 in bsdrX). `voice_sr` is the voice
 * model's native output rate (32000/40000/48000; 0 = default 40000). Loads on the calling thread
 * (can take a second); returns NULL on failure (caller falls back to DSP). */
bsdr_voiceai *bsdr_voiceai_open(int tier, const char *content_onnx, const char *rmvpe_onnx,
                                const char *voice_onnx, int io_sample_rate, int voice_sr);

/* 1 once all sessions are loaded and inference is running (until then, process() passes through). */
int bsdr_voiceai_ready(const bsdr_voiceai *v);

/* Streaming convert `frames` mono int16 samples: reads `in`, writes `frames` samples to `out`
 * (delayed, converted). Buffers internally. `key_semitones` shifts pitch (e.g. +12 up an octave).
 * A safe passthrough (out=in) until warmed up or if inference errors. */
void bsdr_voiceai_process(bsdr_voiceai *v, const int16_t *in, int16_t *out, int frames, float key_semitones);

const char *bsdr_voiceai_status(const bsdr_voiceai *v);
void bsdr_voiceai_close(bsdr_voiceai *v);

#endif /* BSDR_VOICEAI_H */
