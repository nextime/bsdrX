/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
/* Speech-to-text: posts audio to an OpenAI-compatible transcription endpoint
 * (POST <endpoint>  multipart: file=<wav>, model). Works with a local
 * whisper-server or a remote cloud service — just point `endpoint` at it.
 * Default is remote (configured in app state). */
#ifndef BSDR_STT_H
#define BSDR_STT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Built-in free online transcription service, used when no `endpoint` is set.
 * HuggingFace serverless inference for Whisper: free tier, returns {"text":...}.
 * It takes the raw audio body (not OpenAI multipart) — bsdr_stt_transcribe
 * auto-detects a huggingface host and posts the WAV bytes directly. A free HF
 * token (pasted in the STT token field) lifts the anonymous rate limit. */
#define BSDR_STT_FREE_ENDPOINT \
    "https://api-inference.huggingface.co/models/openai/whisper-large-v3-turbo"

typedef struct {
    char endpoint[256];   /* full URL, e.g. https://.../v1/audio/transcriptions
                             or http://127.0.0.1:8080/inference (local whisper) */
    char token[256];      /* bearer token (optional; for cloud) */
    char model[64];       /* e.g. "whisper-1" */
} bsdr_stt_config;

/* Transcribe interleaved int16 PCM (`frames` samples/channel at `rate` Hz,
 * `channels`). Writes UTF-8 text to `out`. Returns true on success. */
bool bsdr_stt_transcribe(const bsdr_stt_config *cfg,
                         const int16_t *pcm, int frames, int rate, int channels,
                         char *out, size_t out_len);

#endif /* BSDR_STT_H */
