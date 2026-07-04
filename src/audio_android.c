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
/* Android device half of include/bsdr/audio.h. The portable Opus RTP/SRTP
 * sender/receiver stays in audio.c; this provides the bsdr_pa_* / virtual-device
 * surface against Android's audio path (Kotlin AudioRecord/AudioTrack via JNI):
 *
 *   - Desktop-out (system audio -> Quest): Kotlin AudioPlaybackCapture pushes
 *     interleaved int16 PCM in via bsdr_android_push_audio; bsdr_pa_record_open's
 *     handle buffers it and bsdr_pa_read drains it for the Opus encoder.
 *   - Mic-in (Quest mic -> device): the decoded PCM written via bsdr_pa_write is
 *     forwarded to Kotlin AudioTrack (bsdr_android_emit_mic). Android has no
 *     virtual microphone without root, so it plays out / feeds STT (see ANDROID.md).
 *   - bsdr_audio_devices_* is a no-op: there is no "silence the speakers" or
 *     pactl-style virtual device on a non-root Android. */
#include "bsdr/audio.h"
#include "bsdr/protocol.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr_android.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct bsdr_pa {
    int dir;            /* 0 = record (system audio in), 1 = playback (mic out) */
    int channels;
    int16_t *ring;      /* record ring, fed by bsdr_android_push_audio */
    size_t cap, head, tail, count;
    bsdr_mutex *lock;
};

/* the active record handle that JNI-pushed system audio lands in */
static bsdr_pa   *g_record;
static bsdr_mutex *g_lock;

void bsdr_android_audio_init(void) {
    if (!g_lock) g_lock = bsdr_mutex_new();
}

bsdr_pa *bsdr_pa_record_open(const char *source, int channels) {
    (void)source;
    bsdr_android_audio_init();
    bsdr_pa *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->dir = 0; p->channels = channels;
    p->cap  = (size_t)BSDR_AUDIO_CLOCK_HZ * (channels > 0 ? channels : 1);  /* ~1 s */
    p->ring = calloc(p->cap, sizeof(int16_t));
    p->lock = bsdr_mutex_new();
    if (!p->ring || !p->lock) { bsdr_pa_close(p); return NULL; }
    bsdr_mutex_lock(g_lock); g_record = p; bsdr_mutex_unlock(g_lock);
    BSDR_INFO("bsdr.audio", "android record open (%d ch) <- AudioPlaybackCapture", channels);
    return p;
}

bsdr_pa *bsdr_pa_play_open(const char *sink, int channels) {
    (void)sink;
    bsdr_pa *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->dir = 1; p->channels = channels;
    BSDR_INFO("bsdr.audio", "android play open (%d ch) -> AudioTrack", channels);
    return p;
}

int bsdr_pa_read(bsdr_pa *pa, int16_t *pcm, int frames) {
    size_t need = (size_t)frames * pa->channels, got = 0;
    /* block (like pulse) until a frame's worth is available, with a cap so a
     * stalled capture can't wedge the encoder thread */
    for (int spins = 0; got < need && spins < 250; spins++) {
        bsdr_mutex_lock(pa->lock);
        while (got < need && pa->count > 0) {
            pcm[got++] = pa->ring[pa->tail];
            pa->tail = (pa->tail + 1) % pa->cap; pa->count--;
        }
        bsdr_mutex_unlock(pa->lock);
        if (got < need) bsdr_sleep_ms(2);
    }
    if (got < need) memset(pcm + got, 0, (need - got) * sizeof(int16_t));  /* underrun */
    return frames;
}

int bsdr_pa_write(bsdr_pa *pa, const int16_t *pcm, int frames) {
    bsdr_android_emit_mic(pcm, frames, pa->channels);   /* -> Kotlin AudioTrack */
    return frames;
}

void bsdr_pa_close(bsdr_pa *pa) {
    if (!pa) return;
    if (pa->dir == 0 && g_lock) {
        bsdr_mutex_lock(g_lock);
        if (g_record == pa) g_record = NULL;
        bsdr_mutex_unlock(g_lock);
    }
    if (pa->lock) bsdr_mutex_free(pa->lock);
    free(pa->ring);
    free(pa);
}

void bsdr_android_push_audio(const int16_t *pcm, int frames, int channels) {
    if (!g_lock || !pcm) return;
    bsdr_mutex_lock(g_lock);
    bsdr_pa *p = g_record;
    if (p) {
        bsdr_mutex_lock(p->lock);
        size_t n = (size_t)frames * channels;
        for (size_t i = 0; i < n; i++) {
            if (p->count == p->cap) {                   /* overflow: drop oldest */
                p->tail = (p->tail + 1) % p->cap; p->count--;
            }
            p->ring[p->head] = pcm[i];
            p->head = (p->head + 1) % p->cap; p->count++;
        }
        bsdr_mutex_unlock(p->lock);
    }
    bsdr_mutex_unlock(g_lock);
}

/* No virtual devices on non-root Android: capture is MediaProjection-scoped and
 * there's no system virtual mic. Report success with placeholder names so the
 * agent's audio threads run; the device names are unused by the shims. */
bool bsdr_audio_devices_create(bsdr_audio_devices *d) {
    memset(d, 0, sizeof(*d));
    d->speaker_module = d->mic_sink_module = d->mic_source_module = -1;
    snprintf(d->monitor_source, sizeof(d->monitor_source), "android-playback-capture");
    snprintf(d->mic_sink, sizeof(d->mic_sink), "android-mic");
    d->active = true;
    BSDR_INFO("bsdr.audio", "android audio: AudioPlaybackCapture in, AudioTrack out (no virtual mic)");
    return true;
}

void bsdr_audio_devices_destroy(bsdr_audio_devices *d) {
    if (d) d->active = false;
}

void bsdr_audio_cleanup_stale_devices(void) { /* AAudio: no virtual modules */ }
