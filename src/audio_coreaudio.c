/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* macOS audio I/O backend (CoreAudio / AudioToolbox) — the platform half of audio.h, mirroring
 * audio_wasapi.c (Windows) and audio_android.c (Android). The platform-independent Opus sender,
 * receiver (jitter buffer) and threaded player live in audio.c and are shared; this file provides
 * only the device I/O: bsdr_pa_* (record/playback via AudioQueue), the virtual sink/source
 * (bsdr_audio_devices_*), and the standalone owner-mic device (bsdr_virtual_mic_*).
 *
 * macOS has no built-in loopback, so — like VB-CABLE on Windows — the user installs BlackHole
 * (https://existential.audio/blackhole/, `brew install blackhole-2ch`):
 *   - Desktop audio OUT (PC -> Quest): route the system output to BlackHole (via a Multi-Output
 *     Device so you still hear it) and we capture from the BlackHole input.
 *   - Mic IN (Quest/owner -> PC): we render the decoded voice into BlackHole; apps then record it
 *     from the BlackHole input as a microphone.
 * With a single BlackHole 2ch device these two paths (and the room-mic vs owner-mic split) share
 * one loopback; for simultaneous independent use, install a second loopback (e.g. BlackHole 16ch)
 * and pass its name. Device names are matched by substring.
 *
 * NOTE: written to the documented CoreAudio APIs; verify with a compile+run on macOS.
 */
#include "bsdr/audio.h"
#include "bsdr/log.h"
#include "bsdr/protocol.h"   /* BSDR_AUDIO_CLOCK_HZ */

#if defined(__APPLE__) && defined(BSDR_HAVE_AUDIO)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#define AQ_BUFFERS 3
#define AQ_FRAMES  480                     /* 10 ms @ 48 kHz per buffer */
#define BLACKHOLE  "BlackHole"

/* Find an AudioDevice by name substring in the given scope. Fills uid; returns 0 on success. */
static int find_device(const char *name_substr, char *uid, size_t uidlen, AudioDeviceID *out) {
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &sz) != noErr) return -1;
    int n = (int)(sz / sizeof(AudioDeviceID));
    AudioDeviceID *ids = malloc(sz ? sz : 1);
    if (!ids) return -1;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, ids) != noErr) { free(ids); return -1; }
    int found = -1;
    for (int i = 0; i < n && found < 0; i++) {
        CFStringRef nm = NULL; UInt32 s = sizeof nm;
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &np, 0, NULL, &s, &nm) != noErr || !nm) continue;
        char buf[128]; CFStringGetCString(nm, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(nm);
        if (strcasestr(buf, name_substr)) {
            CFStringRef u = NULL; s = sizeof u;
            AudioObjectPropertyAddress up = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            if (AudioObjectGetPropertyData(ids[i], &up, 0, NULL, &s, &u) == noErr && u) {
                CFStringGetCString(u, uid, uidlen, kCFStringEncodingUTF8); CFRelease(u);
            }
            *out = ids[i]; found = 0;
        }
    }
    free(ids);
    return found;
}

/* -------------------------------------------------------------- bsdr_pa ---*/

struct bsdr_pa {
    AudioQueueRef       q;
    AudioQueueBufferRef bufs[AQ_BUFFERS];
    int                 channels, record;
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
    int16_t            *ring;
    int                 cap, head, tail, count;    /* samples */
    int                 running;
};

static void asbd(AudioStreamBasicDescription *f, int ch) {
    memset(f, 0, sizeof *f);
    f->mSampleRate = BSDR_AUDIO_CLOCK_HZ; f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f->mBitsPerChannel = 16; f->mChannelsPerFrame = (UInt32)ch;
    f->mFramesPerPacket = 1; f->mBytesPerFrame = (UInt32)(2 * ch); f->mBytesPerPacket = f->mBytesPerFrame;
}

/* set the queue's device to the named one (in/out), best-effort */
static void bind_device(AudioQueueRef q, const char *name) {
    if (!name || !name[0]) return;
    char uid[128] = ""; AudioDeviceID dev = 0;
    if (find_device(name, uid, sizeof uid, &dev) == 0 && uid[0]) {
        CFStringRef u = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
        AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &u, sizeof u);
        CFRelease(u);
    } else {
        BSDR_WARN("bsdr.audio", "CoreAudio: device '%s' not found; using system default", name);
    }
}

static void out_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    struct bsdr_pa *pa = user;
    int want = AQ_FRAMES * pa->channels, got = 0;
    int16_t *out = (int16_t *)b->mAudioData;
    pthread_mutex_lock(&pa->lock);
    while (got < want && pa->count > 0) { out[got++] = pa->ring[pa->head]; pa->head = (pa->head + 1) % pa->cap; pa->count--; }
    pthread_mutex_unlock(&pa->lock);
    while (got < want) out[got++] = 0;
    b->mAudioDataByteSize = (UInt32)(want * 2);
    if (pa->running) AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void in_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts,
                  UInt32 npkt, const AudioStreamPacketDescription *pd) {
    (void)ts; (void)npkt; (void)pd;
    struct bsdr_pa *pa = user;
    int have = (int)(b->mAudioDataByteSize / 2);
    const int16_t *in = (const int16_t *)b->mAudioData;
    pthread_mutex_lock(&pa->lock);
    for (int i = 0; i < have; i++) {
        if (pa->count >= pa->cap) { pa->head = (pa->head + 1) % pa->cap; pa->count--; }
        pa->ring[pa->tail] = in[i]; pa->tail = (pa->tail + 1) % pa->cap; pa->count++;
    }
    pthread_cond_signal(&pa->cond);
    pthread_mutex_unlock(&pa->lock);
    if (pa->running) AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static bsdr_pa *pa_new(const char *dev, int channels, int record) {
    struct bsdr_pa *pa = calloc(1, sizeof *pa);
    if (!pa) return NULL;
    pa->channels = channels > 0 ? channels : 1; pa->record = record;
    pa->cap = BSDR_AUDIO_CLOCK_HZ * pa->channels;   /* ~1 s */
    pa->ring = calloc((size_t)pa->cap, sizeof(int16_t));
    pthread_mutex_init(&pa->lock, NULL); pthread_cond_init(&pa->cond, NULL);
    AudioStreamBasicDescription f; asbd(&f, pa->channels);
    OSStatus rc = record ? AudioQueueNewInput(&f, in_cb, pa, NULL, NULL, 0, &pa->q)
                         : AudioQueueNewOutput(&f, out_cb, pa, NULL, NULL, 0, &pa->q);
    if (rc != noErr) { BSDR_ERROR("bsdr.audio", "CoreAudio: AudioQueueNew%s failed (%d)", record?"Input":"Output", (int)rc);
                       free(pa->ring); free(pa); return NULL; }
    bind_device(pa->q, dev);
    pa->running = 1;
    for (int i = 0; i < AQ_BUFFERS; i++) {
        AudioQueueAllocateBuffer(pa->q, (UInt32)(AQ_FRAMES * 2 * pa->channels), &pa->bufs[i]);
        if (record) AudioQueueEnqueueBuffer(pa->q, pa->bufs[i], 0, NULL);
        else out_cb(pa, pa->q, pa->bufs[i]);        /* prime with silence */
    }
    AudioQueueStart(pa->q, NULL);
    return pa;
}

bsdr_pa *bsdr_pa_record_open(const char *source, int channels) { return pa_new(source, channels, 1); }
bsdr_pa *bsdr_pa_play_open(const char *sink, int channels)     { return pa_new(sink,   channels, 0); }
/* No noisy backend error to suppress here (the loopback device is persistent) -> same as _play_open. */
bsdr_pa *bsdr_pa_play_open_quiet(const char *sink, int channels){ return pa_new(sink,   channels, 0); }

int bsdr_pa_read(bsdr_pa *pa, int16_t *pcm, int frames) {
    if (!pa) return -1;
    int want = frames * pa->channels;
    pthread_mutex_lock(&pa->lock);
    while (pa->count < want && pa->running) pthread_cond_wait(&pa->cond, &pa->lock);
    int got = 0; while (got < want && pa->count > 0) { pcm[got++] = pa->ring[pa->head]; pa->head = (pa->head + 1) % pa->cap; pa->count--; }
    pthread_mutex_unlock(&pa->lock);
    while (got < want) pcm[got++] = 0;
    return frames;
}
int bsdr_pa_write(bsdr_pa *pa, const int16_t *pcm, int frames) {
    if (!pa) return -1;
    int n = frames * pa->channels;
    pthread_mutex_lock(&pa->lock);
    for (int i = 0; i < n; i++) {
        if (pa->count >= pa->cap) { pa->head = (pa->head + 1) % pa->cap; pa->count--; }
        pa->ring[pa->tail] = pcm[i]; pa->tail = (pa->tail + 1) % pa->cap; pa->count++;
    }
    pthread_mutex_unlock(&pa->lock);
    return frames;
}
void bsdr_pa_close(bsdr_pa *pa) {
    if (!pa) return;
    pa->running = 0;
    pthread_mutex_lock(&pa->lock); pthread_cond_broadcast(&pa->cond); pthread_mutex_unlock(&pa->lock);
    if (pa->q) { AudioQueueStop(pa->q, true); AudioQueueDispose(pa->q, true); }
    pthread_mutex_destroy(&pa->lock); pthread_cond_destroy(&pa->cond);
    free(pa->ring); free(pa);
}

/* --------------------------------------------------- virtual sink/source ---*/

/* Desktop-audio capture + Quest-mic playback both go through BlackHole (see the file header for the
 * routing + the single-loopback caveat). We don't create devices on macOS — the user installs
 * BlackHole — so this just resolves names and warns if it's missing. */
bool bsdr_audio_devices_create(bsdr_audio_devices *d) {
    memset(d, 0, sizeof *d);
    d->speaker_module = d->mic_sink_module = d->mic_source_module = -1;
    char uid[128] = ""; AudioDeviceID dev = 0;
    bool have = (find_device(BLACKHOLE, uid, sizeof uid, &dev) == 0);
    snprintf(d->monitor_source, sizeof d->monitor_source, "%s", BLACKHOLE);   /* capture desktop audio */
    snprintf(d->mic_sink,       sizeof d->mic_sink,       "%s", BLACKHOLE);   /* play Quest mic here   */
    if (!have)
        BSDR_WARN("bsdr.audio", "BlackHole not found — install it (brew install blackhole-2ch) for "
                  "desktop-audio share and the Quest virtual mic. Route system output to BlackHole "
                  "via a Multi-Output Device so you still hear audio.");
    else
        BSDR_INFO("bsdr.audio", "CoreAudio devices: desktop capture + Quest mic via BlackHole");
    d->active = true;
    return true;
}
void bsdr_audio_devices_destroy(bsdr_audio_devices *d) { if (d) d->active = false; }
void bsdr_audio_cleanup_stale_devices(void) { /* nothing persistent to clean on macOS */ }

/* Standalone owner-mic device for the sniffer: BlackHole again (see header). */
bool bsdr_virtual_mic_create(const char *sink_name, const char *source_name,
                             const char *description, int *sink_module, int *source_module) {
    (void)sink_name; (void)source_name;
    if (sink_module) *sink_module = -1;
    if (source_module) *source_module = -1;
    char uid[128] = ""; AudioDeviceID dev = 0;
    if (find_device(BLACKHOLE, uid, sizeof uid, &dev) != 0) {
        BSDR_ERROR("bsdr.audio", "%s needs BlackHole installed (brew install blackhole-2ch) — the "
                   "decoded voice is played to BlackHole; record it from the BlackHole input.", description);
        return false;
    }
    BSDR_INFO("bsdr.audio", "virtual mic: routing %s to BlackHole (%s)", description, uid);
    return true;
}
void bsdr_virtual_mic_destroy(int sink_module, int source_module) { (void)sink_module; (void)source_module; }

#endif /* __APPLE__ && BSDR_HAVE_AUDIO */
