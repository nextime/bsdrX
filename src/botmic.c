/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Bot -> cloud room mic producer — see bsdr/botmic.h. */
#include "bsdr/botmic.h"
#include "bsdr/log.h"

#if defined(BSDR_ENABLE_AUDIO) && defined(BSDR_HAVE_AUDIO)

#include "bsdr/audio.h"
#include "bsdr/udp_transport.h"
#include "bsdr/cloud.h"
#include "bsdr/protocol.h"
#include "bsdr/platform.h"
#include "bsdr/plugin.h"   /* keepalive cadence override may come from a loadable plugin */

#include <stdlib.h>
#include <string.h>

#define BOTMIC_BPS 32000   /* voice bitrate — matches the real client's BigMicStream (32 kbps mono) */
#define BOTMIC_FRAME 480   /* 10 ms @ 48 kHz mono — one Opus frame; matches BigMicStream (was 20 ms) */

struct bsdr_botmic {
    bsdr_udp           udp;
    bsdr_audio_sender *tx;
    bsdr_mutex        *lock;   /* push() may be called from the TTS/compctl thread */
    int                up;
    /* Continuous-stream keepalive: a real Bigscreen client ALWAYS streams Opus (even muted). If the
     * bot only sent audio while speaking, its producer goes silent/dead between utterances — the
     * consuming Quest then tears down the FMOD channel for it, and a mod that applies a per-user
     * volume to that channel (Bigscreen Behind's UserVolume) can lock a just-destroyed mutex and
     * crash. So a thread streams gapless 10 ms silence frames (paced to wall-clock) whenever TTS
     * isn't, keeping the producer — and the consumer's FMOD channel — continuously fed. */
    bsdr_thread       *keepalive;
    volatile int       stop;
    volatile uint64_t  last_push_ms;
    const volatile int *keepalive_on;   /* borrowed live flag; 0 disables the silence stream (debug) */
};

static void botmic_keepalive_fn(void *arg) {
    struct bsdr_botmic *b = (struct bsdr_botmic *)arg;
    static const int16_t silence[BOTMIC_FRAME] = { 0 };
    /* Stream one 10 ms Opus frame every 10 ms of WALL-CLOCK, paced against a running clock so the
     * producer's RTP timestamp tracks real time exactly — a real BigMicStream::Broadcast emits gapless
     * 10 ms frames at 10 ms cadence. The old code sent one 10 ms frame per 20 ms sleep = HALF real-time:
     * the consuming Quest's Opus/FMOD channel starved and underran, and an underrun teardown races the
     * FMOD mixer -> pthread_mutex_lock on a destroyed mutex -> SIGABRT. Pacing to the clock (and only
     * filling gaps TTS didn't) keeps the channel fed, which is what "continuous like a real client"
     * was always meant to do.
     *
     * A loadable plugin may override the cadence (bsdr_plugins_mic_keepalive_period_ms) — e.g. the
     * private "legacy-mic" plugin restores the old 20 ms pacing for older Quest builds that expect it.
     * With no such plugin the period is the correct 10 ms; queried live so a plugin toggle applies
     * without a reconnect. */
    uint64_t next = bsdr_now_ms();
    while (!b->stop) {
        uint64_t now = bsdr_now_ms();
        int on = b->keepalive_on ? *b->keepalive_on : 1;
        const uint64_t PERIOD = (uint64_t)bsdr_plugins_mic_keepalive_period_ms(10);  /* ms per frame slot */
        /* Emit one frame per elapsed 10 ms slot, catching up if the sleep overslept. */
        while (now >= next && !b->stop) {
            /* Defer to TTS: if it pushed real audio within this slot, let it drive the stream and just
             * advance the clock (don't double up the frame rate). */
            if (on && (now - b->last_push_ms) >= PERIOD && b->tx) {
                if (b->lock) bsdr_mutex_lock(b->lock);
                if (b->tx && !b->stop) bsdr_audio_send_pcm(b->tx, silence, BOTMIC_FRAME);
                if (b->lock) bsdr_mutex_unlock(b->lock);
            }
            next += PERIOD;
        }
        /* Don't spew a huge catch-up burst after a long stall (e.g. a TTS burst or scheduler hiccup). */
        if (next + 100 < now) next = now;
        bsdr_sleep_ms(5);   /* half a frame — enough resolution to hold 10 ms cadence */
    }
}

bsdr_botmic *bsdr_botmic_start(const char *relay_ip, int mic_port, const char *session_id,
                               const volatile int *keepalive_on) {
    if (!relay_ip || !relay_ip[0] || mic_port <= 0) return NULL;
    struct bsdr_botmic *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->keepalive_on = keepalive_on;
    /* Ephemeral local port; comedia = the relay latches our address from our first RTP packet. */
    if (!bsdr_udp_open(&b->udp, 0, relay_ip, (uint16_t)mic_port)) {
        BSDR_WARN("bsdr.botmic", "udp open -> %s:%d failed", relay_ip, mic_port);
        free(b); return NULL;
    }
    /* SSRC = djb2(the bot's own userSessionId) — the deterministic mediasoup producer registration
     * (same model as the companion's cloud audio). PT 100, 48 kHz MONO, plain RTP + cloud trailer. */
    uint32_t ssrc = (session_id && session_id[0]) ? bsdr_cloud_user_ssrc(session_id) : BSDR_AUDIO_SSRC;
    b->tx = bsdr_audio_sender_new(&b->udp, NULL /*plain RTP*/, BSDR_AUDIO_PT_DEFAULT, ssrc,
                                  1 /*mono voice*/, BOTMIC_BPS);
    if (!b->tx) { BSDR_WARN("bsdr.botmic", "opus sender init failed"); bsdr_udp_close(&b->udp); free(b); return NULL; }
    bsdr_audio_sender_enable_cloud_trailer(b->tx);   /* [u32 ssrc LE][u32 frame_id LE] after Opus */
    bsdr_audio_sender_set_dup(b->tx, 2);             /* real room mic sends each 10 ms frame twice */
    b->lock = bsdr_mutex_new();
    b->up = 1;
    b->last_push_ms = bsdr_now_ms();
    b->keepalive = bsdr_thread_start(botmic_keepalive_fn, b);   /* continuous stream, like a real client */
    BSDR_INFO("bsdr.botmic", "room-mic producer up -> %s:%d (pt=%u ssrc=%08x, mono, continuous)",
              relay_ip, mic_port, BSDR_AUDIO_PT_DEFAULT, ssrc);
    return b;
}

int bsdr_botmic_push(bsdr_botmic *b, const int16_t *pcm, int frames) {
    if (!b || !b->up || !pcm || frames <= 0) return -1;
    int rc;
    if (b->lock) bsdr_mutex_lock(b->lock);
    rc = bsdr_audio_send_pcm(b->tx, pcm, frames);
    if (b->lock) bsdr_mutex_unlock(b->lock);
    b->last_push_ms = bsdr_now_ms();   /* the keepalive defers to real audio for the next frame */
    return rc;
}

void bsdr_botmic_stop(bsdr_botmic *b) {
    if (!b) return;
    b->up = 0;
    b->stop = 1;
    if (b->keepalive) { bsdr_thread_join(b->keepalive); b->keepalive = NULL; }   /* stop streaming before freeing tx */
    if (b->lock) bsdr_mutex_lock(b->lock);
    if (b->tx) { bsdr_audio_sender_free(b->tx); b->tx = NULL; }
    if (b->lock) bsdr_mutex_unlock(b->lock);
    bsdr_udp_close(&b->udp);
    if (b->lock) bsdr_mutex_free(b->lock);
    free(b);
    BSDR_INFO("bsdr.botmic", "room-mic producer stopped");
}

#else  /* no audio/media build */

bsdr_botmic *bsdr_botmic_start(const char *relay_ip, int mic_port, const char *session_id,
                               const volatile int *keepalive_on) {
    (void)relay_ip; (void)mic_port; (void)session_id; (void)keepalive_on;
    BSDR_WARN("bsdr.botmic", "room-mic producer needs the audio build; unavailable");
    return NULL;
}
int  bsdr_botmic_push(bsdr_botmic *b, const int16_t *pcm, int frames) { (void)b; (void)pcm; (void)frames; return -1; }
void bsdr_botmic_stop(bsdr_botmic *b) { (void)b; }

#endif
