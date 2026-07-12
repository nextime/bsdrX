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
/* Cloud-mic loopback via the bot — see bsdr/botaudio.h. */
#include "bsdr/botaudio.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BSDR_ENABLE_AUDIO   /* the loopback needs the Opus RTP receiver + virtual-mic device (audio.c) */

#include "bsdr/cloud.h"
#include "bsdr/udp_transport.h"
#include "bsdr/audio.h"
#include "bsdr/protocol.h"
#include "bsdr/roommic.h"
#include "bsdr/platform.h"

#if defined(BSDR_PLATFORM_ANDROID)
void bsdr_android_emit_room(const int16_t *pcm, int frames, int channels);
#endif

struct bsdr_botaudio {
    bsdr_thread *th;
    volatile int stop;
    char ip[64];
    int  audio_port;
    uint32_t owner_ssrc;   /* djb2(ownerSessionId), 0 if unknown — the "listen only to me" solo */
    bsdr_app *app;
};

/* Render one mixed room frame -> BSDR_RoomMic (and the Android room sink). */
struct ba_cb { bsdr_audio_player *play; bsdr_app *app; };
static void ba_pcm_cb(const int16_t *pcm, int frames, int channels, void *user) {
    struct ba_cb *c = (struct ba_cb *)user;
#if !defined(BSDR_PLATFORM_ANDROID)
    if (c->play) bsdr_audio_player_push(c->play, pcm, frames);
#else
    if (c->app && c->app->room_mic_want) bsdr_android_emit_room(pcm, frames, channels);
#endif
    (void)channels;
}

static void botaudio_thread(void *arg) {
    struct bsdr_botaudio *b = (struct bsdr_botaudio *)arg;
    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, 0, b->ip, (uint16_t)b->audio_port)) {
        BSDR_WARN("bsdr.botaudio", "loopback: udp -> %s:%d failed", b->ip, b->audio_port);
        return;
    }
    /* Comedia: the relay latches onto our source tuple from the first packet, so announce ourselves
     * before expecting the room mix, and keep a slow keepalive so the latch/NAT mapping stays live. */
    uint8_t z = 0;
    bsdr_udp_send(&udp, &z, 1);

    struct ba_cb ctx = { NULL, b->app };
    bsdr_audio_recv *rx = bsdr_audio_recv_new(NULL /*plain*/, BSDR_AUDIO_PT_DEFAULT, BSDR_AUDIO_SSRC,
                                              BSDR_AUDIO_CHANNELS, ba_pcm_cb, &ctx);
    if (!rx) { bsdr_udp_close(&udp); return; }
    bsdr_audio_recv_enable_cloud_trailer(rx);   /* room audio carries the 8-byte [ssrc][frame_id] trailer */
    BSDR_INFO("bsdr.botaudio", "loopback: consuming room audio %s:%d -> BSDR_RoomMic%s",
              b->ip, b->audio_port, b->owner_ssrc ? " (owner solo available)" : "");

#if !defined(BSDR_PLATFORM_ANDROID)
    bsdr_audio_player *play = NULL; int rm_sink = -1, rm_src = -1, dev_up = 0;
#endif
    uint8_t buf[2048];
    uint64_t next_play = bsdr_now_ms(), last_ka = next_play;
    while (!b->stop) {
        int want_dev = b->app && b->app->room_mic_want;
#if !defined(BSDR_PLATFORM_ANDROID)
        if (want_dev && !dev_up) {
            if (bsdr_virtual_mic_create(ROOM_SINK, ROOM_MIC_SINK_NAME, ROOM_MIC_LABEL, &rm_sink, &rm_src))
                play = bsdr_audio_player_new(ROOM_SINK, BSDR_AUDIO_CHANNELS);
            dev_up = 1;
            if (b->app) b->app->bot_roommic_active = 1;   /* host cloud_mic_main defers to us */
            BSDR_INFO("bsdr.botaudio", "loopback: BSDR_RoomMic up (bot-sourced, includes your own voice)");
        } else if (!want_dev && dev_up) {
            if (play) { bsdr_audio_player_free(play); play = NULL; }
            bsdr_virtual_mic_destroy(rm_sink, rm_src); rm_sink = rm_src = -1; dev_up = 0;
            if (b->app) b->app->bot_roommic_active = 0;
        }
        ctx.play = play;
#endif
        /* Live solo: "listen only to me" mutes every SSRC but the room owner's. */
        int solo = b->app && b->app->bot_solo_owner && b->owner_ssrc;
        bsdr_audio_recv_set_solo(rx, solo ? b->owner_ssrc : 0);

        int n = bsdr_udp_recv(&udp, buf, sizeof buf, 5);
        if (n > 0) bsdr_audio_recv_feed(rx, buf, n);
        uint64_t now = bsdr_now_ms();
        while ((int64_t)(now - next_play) >= 0) { bsdr_audio_recv_playout(rx); next_play += 20; }
        if (now - last_ka >= 1000) { bsdr_udp_send(&udp, &z, 1); last_ka = now; }  /* latch keepalive */
    }
#if !defined(BSDR_PLATFORM_ANDROID)
    if (play) bsdr_audio_player_free(play);
    if (dev_up) bsdr_virtual_mic_destroy(rm_sink, rm_src);
#endif
    if (b->app) b->app->bot_roommic_active = 0;
    bsdr_audio_recv_free(rx);
    bsdr_udp_close(&udp);
    BSDR_INFO("bsdr.botaudio", "loopback: stopped");
}

bsdr_botaudio *bsdr_botaudio_start(const char *relay_ip, int audio_port,
                                   const char *owner_session_id, bsdr_app *app) {
    if (!relay_ip || !relay_ip[0] || audio_port <= 0) return NULL;
    struct bsdr_botaudio *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    snprintf(b->ip, sizeof b->ip, "%s", relay_ip);
    b->audio_port = audio_port;
    b->owner_ssrc = (owner_session_id && owner_session_id[0]) ? bsdr_cloud_user_ssrc(owner_session_id) : 0;
    b->app = app;
    b->th = bsdr_thread_start(botaudio_thread, b);
    if (!b->th) { free(b); return NULL; }
    return b;
}

void bsdr_botaudio_stop(bsdr_botaudio *b) {
    if (!b) return;
    b->stop = 1;
    if (b->th) bsdr_thread_join(b->th);
    if (b->app) b->app->bot_roommic_active = 0;
    free(b);
}

#else  /* !BSDR_ENABLE_AUDIO — no Opus receiver: cloud-mic loopback unavailable. */

bsdr_botaudio *bsdr_botaudio_start(const char *relay_ip, int audio_port,
                                   const char *owner_session_id, bsdr_app *app) {
    (void)relay_ip; (void)audio_port; (void)owner_session_id; (void)app;
    BSDR_WARN("bsdr.botaudio", "cloud-mic loopback needs the audio build; unavailable");
    return NULL;
}
void bsdr_botaudio_stop(bsdr_botaudio *b) { (void)b; }

#endif
