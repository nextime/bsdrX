/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Voice pipeline state machine (no network / no injector): a triggered capture
 * with no speech returns to IDLE, and stop_capture -> CONFIRM -> Cancel returns
 * to IDLE. State-callback transitions are recorded. */
#include "bsdr/voice.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <string.h>

static int last_state = -1, saw_listening = 0, saw_confirm = 0;
static void on_state(void *u, int s) { (void)u; last_state = s;
    if (s == BSDR_VST_LISTENING) saw_listening = 1;
    if (s == BSDR_VST_CONFIRM)   saw_confirm = 1; }

static int wait_state(bsdr_voice *v, int want, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 20) {
        if (bsdr_voice_state_get(v) == want) return 1;
        bsdr_sleep_ms(20);
    }
    return 0;
}

int main(void) {
    bsdr_log_set_level(BSDR_LOG_WARN);
    int fail = 0;

    bsdr_voice_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.start_ms = 120; cfg.silence_ms = 120; cfg.max_ms = 1000; cfg.confirm_ms = 800;
    bsdr_voice *v = bsdr_voice_new(&cfg, NULL);   /* no injector: we never reach a tool call */
    bsdr_voice_set_state_cb(v, on_state, NULL);

    if (bsdr_voice_state_get(v) == BSDR_VST_IDLE && !bsdr_voice_busy(v)) printf("PASS initial_idle\n");
    else { printf("FAIL initial_idle\n"); fail++; }

    /* 1) trigger, feed no audio -> LISTENING then (no speech) -> IDLE */
    bsdr_voice_trigger(v, BSDR_VOICE_COMMAND);
    if (wait_state(v, BSDR_VST_LISTENING, 500) || saw_listening) printf("PASS entered_listening\n");
    else { printf("FAIL entered_listening\n"); fail++; }
    if (wait_state(v, BSDR_VST_IDLE, 2000)) printf("PASS no_speech_returns_idle\n");
    else { printf("FAIL no_speech_returns_idle\n"); fail++; }
    char last[256]; bsdr_voice_last(v, last, sizeof last);
    if (strstr(last, "no speech")) printf("PASS no_speech_msg\n");
    else { printf("FAIL no_speech_msg (%s)\n", last); fail++; }

    /* Longer start window so a capture waits for (simulated) speech instead of
     * timing out; the VAD learns its noise floor from the first 250 ms, so we feed
     * quiet first, then a loud burst. */
    int16_t quiet[480] = {0}, loud[480];
    for (int i = 0; i < 480; i++) loud[i] = (int16_t)(((i * 211) % 20000) - 10000);
    cfg.start_ms = 4000;
    bsdr_voice_update_config(v, &cfg);

    /* 2) capture with speech, stop early -> CONFIRM, then Cancel -> IDLE */
    saw_confirm = 0;
    bsdr_voice_trigger(v, BSDR_VOICE_COMMAND);
    wait_state(v, BSDR_VST_LISTENING, 500);
    for (int k = 0; k < 30; k++) { bsdr_voice_push_pcm(v, quiet, 480, 1); bsdr_sleep_ms(10); } /* floor */
    for (int k = 0; k < 25; k++) { bsdr_voice_push_pcm(v, loud, 480, 1);  bsdr_sleep_ms(10); } /* speech */
    bsdr_voice_stop_capture(v);
    if (wait_state(v, BSDR_VST_CONFIRM, 1500) || saw_confirm) printf("PASS stop_to_confirm\n");
    else { printf("FAIL stop_to_confirm (state=%d)\n", bsdr_voice_state_get(v)); fail++; }
    bsdr_voice_confirm(v, false);   /* Cancel */
    if (wait_state(v, BSDR_VST_IDLE, 1000)) printf("PASS cancel_returns_idle\n");
    else { printf("FAIL cancel_returns_idle\n"); fail++; }

    /* 3) confirm auto-cancel on timeout */
    bsdr_voice_trigger(v, BSDR_VOICE_COMMAND);
    wait_state(v, BSDR_VST_LISTENING, 500);
    for (int k = 0; k < 30; k++) { bsdr_voice_push_pcm(v, quiet, 480, 1); bsdr_sleep_ms(10); }
    for (int k = 0; k < 25; k++) { bsdr_voice_push_pcm(v, loud, 480, 1);  bsdr_sleep_ms(10); }
    bsdr_voice_stop_capture(v);
    wait_state(v, BSDR_VST_CONFIRM, 1500);
    if (wait_state(v, BSDR_VST_IDLE, 2000)) printf("PASS confirm_timeout_cancels\n");
    else { printf("FAIL confirm_timeout_cancels\n"); fail++; }

    bsdr_voice_free(v);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - voice state machine passed\n", fail);
    return fail ? 1 : 0;
}
