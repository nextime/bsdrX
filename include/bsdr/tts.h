/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Text-to-speech: turn the LLM computer-control "speak" output into audio. Two engines, selectable at
 * runtime from the web UI:
 *   - LOCAL: the Piper neural TTS CLI (piped: text in, raw s16le PCM out). Offline, no key, low
 *     latency. Needs a `piper` binary on PATH (or --tts-piper) and a voice model (.onnx + .onnx.json).
 *   - CLOUD: an OpenAI-compatible /v1/audio/speech endpoint (model + voice + API key). Best quality.
 *
 * Both normalize to 48 kHz MONO s16 PCM (the room-mic / desktop-audio format) so the caller can route
 * the result to the bot's cloud room mic (bsdr_botmic) by default, or to the desktop sink. Synchronous
 * and self-contained (HTTP via httpc, WAV parse + linear resample here); no ffmpeg dependency. */
#ifndef BSDR_TTS_H
#define BSDR_TTS_H

#include <stdint.h>
#include <stddef.h>

/* BSDR_TTS_FREETTS: freetts.org's public API — zero config, NO API key (POST /api/tts -> file_id,
 * GET /api/audio/{id} -> MP3; free tier 20 req/min, 1000 chars). Decoded via vendored minimp3. */
typedef enum { BSDR_TTS_LOCAL = 0, BSDR_TTS_CLOUD = 1, BSDR_TTS_FREETTS = 2 } bsdr_tts_engine;

typedef struct {
    bsdr_tts_engine engine;
    /* LOCAL (Piper) */
    char piper[512];        /* piper binary (default "piper" on PATH) */
    char model[512];        /* voice model .onnx path */
    /* CLOUD (OpenAI-compatible /v1/audio/speech) */
    char endpoint[256];     /* e.g. https://api.openai.com/v1/audio/speech */
    char token[256];        /* API key (Bearer) */
    char cloud_model[64];   /* e.g. "tts-1" / "gpt-4o-mini-tts" / a provider model */
    char voice[64];         /* voice name: cloud (e.g. "alloy"); ignored by Piper */
} bsdr_tts_config;

/* Synthesize `text` to 48 kHz MONO s16 PCM. On success returns the number of samples (frames) and
 * sets *pcm_out to a malloc'd buffer the caller must free(). Returns <= 0 on failure (and logs why).
 * Blocking; may take a few hundred ms (local) to a couple seconds (cloud). */
int bsdr_tts_synth(const bsdr_tts_config *cfg, const char *text, int16_t **pcm_out);

/* True if this build/host can run the selected engine (local: piper binary + model present; cloud:
 * endpoint + token set). Cheap; for surfacing readiness in the UI. */
int bsdr_tts_available(const bsdr_tts_config *cfg);

#endif /* BSDR_TTS_H */
