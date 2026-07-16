/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* botsense — host-side per-SSRC voice-activity segmentation + delivery worker (ABI 4 bot host-service).
 *
 * It taps the room's per-speaker PCM, runs cheap energy VAD to cut it into utterances, and delivers each
 * complete utterance to a subscribed callback ON ITS OWN WORKER THREAD — so a bot PLUGIN (fullbot) can
 * do STT + wake-word + the LLM loop in that callback without ever touching the audio thread or managing
 * its own (non-portable) threads. This is the mechanical "sensor" half of the old in-core roomcmd; the
 * cognitive half (transcribe, decide, act) lives in the plugin. See PLAN-bot-plugin.md §5, P1.5. */
#ifndef BSDR_BOTSENSE_H
#define BSDR_BOTSENSE_H

#include <stdint.h>
#include "bsdr/plugin.h"   /* bsdr_utterance_cb */

typedef struct bsdr_botsense bsdr_botsense;

/* Create the segmenter + its idle worker thread. Returns NULL on failure. */
bsdr_botsense *bsdr_botsense_new(void);
/* Stop the worker and free everything. Blocks until the worker has exited (no callback in flight). */
void bsdr_botsense_free(bsdr_botsense *b);

/* Set (or clear, with NULL) the utterance-delivery callback. Clearing waits for any in-flight callback
 * to finish before returning, so it's safe to unload the owning plugin right after. */
void bsdr_botsense_set_cb(bsdr_botsense *b, bsdr_utterance_cb cb, void *user);

/* 1 if a delivery callback is currently set (a plugin is subscribed). */
int bsdr_botsense_has_cb(bsdr_botsense *b);

/* Feed one decoded per-speaker PCM frame (the shape of bsdr_ssrc_pcm_cb). Runs VAD; when an utterance
 * ends it is queued for the worker to deliver. Cheap; safe to call from the audio thread. */
void bsdr_botsense_tap(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user);

#endif /* BSDR_BOTSENSE_H */
