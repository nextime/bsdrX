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
/* Voice assistant: on a trigger, capture the headset mic, transcribe (STT), then
 * either type the text (dictate) or run the LLM with computer-control (command).
 * Mic PCM is fed in from the audio receiver. */
#ifndef BSDR_VOICE_H
#define BSDR_VOICE_H

#include "bsdr/stt.h"
#include "bsdr/llm.h"
#include "bsdr/inject.h"
#include <stdint.h>

typedef struct {
    bsdr_stt_config stt;
    bsdr_llm_config llm;
    char system_prompt[1024];
    /* Listen-until-silence VAD (0 => sane defaults are applied):
     *   start_ms  — how long to wait for speech to begin before giving up
     *   silence_ms— trailing quiet after speech that ends the capture
     *   max_ms    — hard cap on a single capture                          */
    int start_ms;
    int silence_ms;
    int max_ms;            /* hard ceiling on one capture (default 5 min) */
    int confirm_ms;        /* how long the Send/Cancel prompt waits (default 30 s) */
    bool vision;           /* offer the model an on-demand desktop screenshot tool */
} bsdr_voice_config;

typedef enum { BSDR_VOICE_COMMAND = 0, BSDR_VOICE_DICTATE = 1 } bsdr_voice_mode;

/* Pipeline state (also the balloon's visual mode). */
typedef enum {
    BSDR_VST_IDLE = 0,     /* nothing happening */
    BSDR_VST_LISTENING,    /* capturing the owner's voice */
    BSDR_VST_CONFIRM,      /* capture stopped; awaiting Send / Cancel */
    BSDR_VST_WORKING       /* transcribing + running the model */
} bsdr_voice_state;

typedef struct bsdr_voice bsdr_voice;

/* Called (on the worker thread) whenever the pipeline state changes. */
typedef void (*bsdr_voice_state_cb)(void *user, int state);
/* Called (on the worker thread) with each status/thinking/result line. */
typedef void (*bsdr_voice_feedback_cb)(void *user, const char *text);
/* Grab a desktop screenshot (JPEG) into `out`; return byte count (0 = none). */
typedef int (*bsdr_voice_shot_cb)(void *user, uint8_t *out, size_t cap);

bsdr_voice *bsdr_voice_new(const bsdr_voice_config *cfg, bsdr_injector *inj);
void bsdr_voice_free(bsdr_voice *v);

/* Refresh the STT/LLM endpoints + system prompt + timings at runtime. */
void bsdr_voice_update_config(bsdr_voice *v, const bsdr_voice_config *cfg);
void bsdr_voice_set_state_cb(bsdr_voice *v, bsdr_voice_state_cb cb, void *user);
void bsdr_voice_set_feedback_cb(bsdr_voice *v, bsdr_voice_feedback_cb cb, void *user);
void bsdr_voice_set_shot_cb(bsdr_voice *v, bsdr_voice_shot_cb cb, void *user);

/* Feed decoded mic PCM (from the owner-mic sniffer); buffered while recording. */
void bsdr_voice_push_pcm(bsdr_voice *v, const int16_t *pcm, int frames, int channels);

/* Start a capture cycle (non-blocking; runs on a worker). */
void bsdr_voice_trigger(bsdr_voice *v, bsdr_voice_mode mode);
/* Stop an active capture early (LISTENING -> CONFIRM). No-op otherwise. */
void bsdr_voice_stop_capture(bsdr_voice *v);
/* Resolve the CONFIRM prompt: send=true runs it, false discards. No-op otherwise. */
void bsdr_voice_confirm(bsdr_voice *v, bool send);
/* Abort the running command loop (WORKING -> stops promptly). No-op otherwise. */
void bsdr_voice_abort(bsdr_voice *v);

int  bsdr_voice_state_get(bsdr_voice *v);   /* current bsdr_voice_state */
bool bsdr_voice_busy(bsdr_voice *v);        /* a cycle is in flight (not IDLE) */

/* Last result text (transcript or LLM reply) for display. */
void bsdr_voice_last(bsdr_voice *v, char *out, size_t len);

#endif /* BSDR_VOICE_H */
