/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Android voice-control bridge. On the desktop the owner-mic sniffer feeds the
 * voice pipeline and an in-VR balloon triggers a listen cycle. Android has no
 * sniffer (no root), so the DEVICE's own microphone is the source and a floating
 * bubble (Kotlin BubbleOverlay) is the trigger. This file is the thread-safe seam
 * between the JNI bridge (Kotlin mic PCM + bubble gestures) and the agent's live
 * bsdr_voice object, which the agent registers here while computer control is armed.
 *
 * The voice->action path is identical to the desktop: bsdr_voice runs STT + the LLM
 * and executes tools through the injector — which on Android is the Accessibility
 * Service (inject_android.c). So once PCM flows and a cycle is triggered, computer
 * control works exactly as it does on Linux. */
#include "bsdr/platform.h"
#include "bsdr/voice.h"
#include "bsdr_android.h"

static bsdr_voice *g_voice;
static bsdr_mutex *g_lock;

static void ensure_lock(void) {
    if (!g_lock) g_lock = bsdr_mutex_new();
}

void bsdr_android_set_voice(struct bsdr_voice *v) {
    ensure_lock();
    bsdr_mutex_lock(g_lock);
    g_voice = v;
    bsdr_mutex_unlock(g_lock);
}

void bsdr_android_push_voice(const int16_t *pcm, int frames) {
    if (!pcm || frames <= 0 || !g_lock) return;
    bsdr_mutex_lock(g_lock);
    if (g_voice) bsdr_voice_push_pcm(g_voice, pcm, frames, 1);   /* mono device mic */
    bsdr_mutex_unlock(g_lock);
}

void bsdr_android_voice_trigger(void) {
    if (!g_lock) return;
    bsdr_mutex_lock(g_lock);
    if (g_voice) bsdr_voice_trigger(g_voice, BSDR_VOICE_COMMAND);
    bsdr_mutex_unlock(g_lock);
}

void bsdr_android_voice_stop(void) {
    if (!g_lock) return;
    bsdr_mutex_lock(g_lock);
    if (g_voice) bsdr_voice_stop_capture(g_voice);
    bsdr_mutex_unlock(g_lock);
}

void bsdr_android_voice_confirm(int send) {
    if (!g_lock) return;
    bsdr_mutex_lock(g_lock);
    if (g_voice) bsdr_voice_confirm(g_voice, send ? true : false);
    bsdr_mutex_unlock(g_lock);
}
