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
/* Cloud (internet) relay streaming to a Mediasoup relay (mediaServer.ip + the
 * video/audio/data ports from GET /rooms). Reversed from BigSoup.dll:
 *   - video (videoPort) + audio (audioPort): PLAIN RTP (jrtplib, NO DTLS/SRTP).
 *     Mediasoup PlainTransport with comedia — we send a 1-byte latch first so the
 *     relay learns our source address, then stream RTP (video pt 111, audio pt 100).
 *   - data (dataPort): DTLS (host = server) carrying the input opcodes.
 * Producer RTP params (pt/ssrc/codec) are otherwise negotiated by the headset
 * when it provisions the room; we match pt 111/100 and a host-chosen SSRC. */
#include "bsdr/cloud_stream.h"
#include "bsdr/app.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/protocol.h"

#ifdef BSDR_HAVE_CAPTURE
#include "bsdr/capture.h"
#include "bsdr/threed.h"
#include "bsdr/fileaudio.h"
#include "bsdr/video.h"
#include "bsdr/inject.h"
#include "bsdr/input_decode.h"
#else
/* Without capture the relay's video path is compiled out, but the struct still
 * declares an (unused) opaque sender pointer — forward-declare the type so this
 * core file builds in input-only configs (e.g. the osxcross / plain-windows cross builds). */
typedef struct bsdr_video_sender bsdr_video_sender;
#endif
#ifdef BSDR_ENABLE_SCTP
#include "bsdr/sctp.h"
#endif
#ifdef BSDR_HAVE_AUDIO
#include "bsdr/audio.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Mediasoup producers are registered with a deterministic SSRC = djb2(userSessionId).
 * BigSoup.dll computes it (FUN_180096e70 -> FUN_180079000): seed 5381, h = h*33 + (signed char)c,
 * 32-bit, over the raw userSessionId string. The relay drops RTP whose SSRC doesn't match the
 * registered producer, so a fixed SSRC never gets forwarded. Logged by the host as
 * "My desktop video SSRC: %u." */
#if defined(BSDR_HAVE_CAPTURE) || defined(BSDR_HAVE_AUDIO)
static uint32_t cloud_ssrc(const char *user_session_id) {
    uint32_t h = 5381;
    for (const char *p = user_session_id; *p; p++)
        h = h * 33u + (uint32_t)(int32_t)(signed char)*p;
    return h;
}
#endif

#define BSDR_CLOUD_VQ 64   /* frame queue depth (~2s @ 30fps); absorbs send jitter without dropping */

struct bsdr_cloud_stream {
    bsdr_app *app;
    bsdr_cloud_screen scr;
    bool audio;
    volatile int stop;
    volatile int active;   /* 1 = sending media; 0 = paused (sockets + SCTP kept alive across toggles) */
    bsdr_thread *vthr, *athr, *mthr, *ithr;
    /* Video is NOT captured here — the LAN path owns the single nvenc encoder and FEEDS us its
     * already-encoded access units (see bsdr_cloud_stream_feed_video). Opening a second encoder
     * for the relay starved the LAN encoder and crashed the Quest. */
    bsdr_video_sender *vid;     /* set up + drained by cloud_video_main's own thread */
    bsdr_udp vid_udp;           /* RTP out + RTCP (muxed) both ways with the SFU */
    uint64_t last_sr;
    volatile int video_ready;   /* frame handoff is live and safe to feed */
    bool decoupled;             /* --video-decoupled: own capture+encoder vs relaying the LAN encode */
    int vid_w, vid_h;           /* encoded resolution for the cloud trailer (set by the feed / own cap) */
    /* FIFO frame queue LAN-encoder -> relay sender. The LAN loop enqueues each encoded access unit
     * (fast memcpy); cloud_video_main's thread dequeues and does the actual relay sends. A FIFO (not
     * a single latest-frame slot) is REQUIRED: dropping P-frames breaks H.264's reference chain, so
     * the Quest can only decode keyframes -> "mostly frozen, a few frames". Keeps the (possibly
     * blocking) internet sends OFF the LAN video loop too. On overflow (uplink can't keep up) the
     * oldest frame is dropped — a brief glitch until the next keyframe, not a permanent freeze. */
    bsdr_mutex *flock;
    struct cloud_vframe { uint8_t *buf; size_t cap, len; } vq[BSDR_CLOUD_VQ];
    int vq_head, vq_tail;       /* head = next to send, tail = next to write */
    long vq_dropped;
};

#ifdef BSDR_HAVE_CAPTURE
/* Enqueue one encoded access unit (no network). Called from the LAN loop under the app lock, so the
 * stream can't be freed mid-copy. In-order, no skipping (except oldest-drop on overflow). */
void bsdr_cloud_stream_feed_video(bsdr_cloud_stream *cs, const uint8_t *au, size_t len, int w, int h) {
    if (!cs || !cs->video_ready || !cs->flock || len == 0) return;
    if (!cs->active) return;               /* paused: drop frames, keep the connection alive */
    cs->vid_w = w; cs->vid_h = h;                    /* LAN encoder resolution -> cloud trailer */
    bsdr_mutex_lock(cs->flock);
    int next = (cs->vq_tail + 1) % BSDR_CLOUD_VQ;
    if (next == cs->vq_head) {                       /* full -> drop the oldest unsent frame */
        cs->vq_head = (cs->vq_head + 1) % BSDR_CLOUD_VQ;
        cs->vq_dropped++;
    }
    struct cloud_vframe *f = &cs->vq[cs->vq_tail];
    if (len > f->cap) { uint8_t *nb = realloc(f->buf, len); if (nb) { f->buf = nb; f->cap = len; } }
    if (f->buf && len <= f->cap) { memcpy(f->buf, au, len); f->len = len; cs->vq_tail = next; }
    bsdr_mutex_unlock(cs->flock);
}
#endif

#if defined(BSDR_HAVE_CAPTURE) || defined(BSDR_HAVE_AUDIO)
/* Comedia latch: Mediasoup PlainTransport learns our source addr from the first
 * packet we send. The host sends a tiny probe before streaming (BigSoup.dll does
 * the same via a 1-byte jrtplib send). Used by both the video (capture) and audio
 * relay paths — macOS builds audio but not capture, so this can't live under
 * BSDR_HAVE_CAPTURE alone. */
static void cloud_latch(bsdr_udp *udp) {
    uint8_t z = 0;
    bsdr_udp_send(udp, &z, 1);
}
#endif

/* ---- source-port selection ------------------------------------------------------------------
 * The Mediasoup relay comedia-latches each transport onto the first source tuple it sees and does
 * NOT re-latch while the screen is presenting. The official client keeps its sockets open across
 * share toggles, so its (ephemeral) source ports never change and the latch stays valid. bsdrX
 * tears the sockets down on each toggle, so with plain ephemeral ports a restart gets NEW ports
 * and the relay ignores us (no INIT-ACK, no audio/data back). Two remedies:
 *   --cloud-src-port N   : hard-pin video=N, audio=N+1, data=N+2.
 *   --cloud-sticky-ports : keep ephemeral (like the official client) on the FIRST stream to a
 *                          relay IP, then REUSE those exact ports for that IP across toggles;
 *                          a new relay IP or a process restart starts fresh. */
static bsdr_mutex *g_sticky_lock = NULL;
static struct { char ip[64]; uint16_t port[3]; } g_sticky;   /* [0]=video [1]=audio [2]=data */

static uint16_t cloud_src_pick(bsdr_app *app, const char *relay_ip, int kind, uint16_t fixed) {
    if (fixed) return fixed;                              /* --cloud-src-port wins */
    if (!app || !app->cloud_sticky_ports) return 0;      /* plain ephemeral */
    if (!g_sticky_lock) g_sticky_lock = bsdr_mutex_new();
    bsdr_mutex_lock(g_sticky_lock);
    if (strcmp(g_sticky.ip, relay_ip) != 0) {            /* relay changed -> forget old tuple */
        snprintf(g_sticky.ip, sizeof(g_sticky.ip), "%s", relay_ip);
        g_sticky.port[0] = g_sticky.port[1] = g_sticky.port[2] = 0;
    }
    uint16_t p = g_sticky.port[kind];                    /* 0 => bind ephemeral, then record it */
    bsdr_mutex_unlock(g_sticky_lock);
    return p;
}
static void cloud_src_record(bsdr_app *app, const char *relay_ip, int kind, uint16_t actual) {
    if (!app || !app->cloud_sticky_ports || !actual || !g_sticky_lock) return;
    bsdr_mutex_lock(g_sticky_lock);
    if (strcmp(g_sticky.ip, relay_ip) == 0) g_sticky.port[kind] = actual;
    bsdr_mutex_unlock(g_sticky_lock);
}

#ifdef BSDR_HAVE_CAPTURE
/* ---- video: relay videoPort, PLAIN RTP (pt 111), NO DTLS/SRTP. We do NOT capture/encode
 * here — that would open a SECOND nvenc session alongside the LAN encoder, starving it and
 * crashing the Quest. Instead set up the RTP sender + comedia latch + RTCP, then idle; the LAN
 * loop feeds us its already-encoded access units via bsdr_cloud_stream_feed_video. ---- */
static void cloud_video_main(void *arg) {
    bsdr_cloud_stream *cs = (bsdr_cloud_stream *)arg;
    uint16_t vfixed = (cs->app && cs->app->cloud_src_port > 0) ? (uint16_t)cs->app->cloud_src_port : 0;
    uint16_t src = cloud_src_pick(cs->app, cs->scr.media_ip, 0, vfixed);
    if (!bsdr_udp_open(&cs->vid_udp, src, cs->scr.media_ip, (uint16_t)cs->scr.video_port)) {
        BSDR_ERROR("bsdr.cloud", "video: udp (src port %u) -> %s:%d failed",
                   src, cs->scr.media_ip, cs->scr.video_port);
        return;
    }
    { uint16_t vport = bsdr_udp_local_port(&cs->vid_udp);
      if (src == 0) cloud_src_record(cs->app, cs->scr.media_ip, 0, vport);
      if (src || (cs->app && cs->app->cloud_sticky_ports))
          BSDR_INFO("bsdr.cloud", "video: source port %u (%s)", vport,
                    vfixed ? "--cloud-src-port" : "sticky"); }
    uint32_t vssrc = cs->scr.session_id[0] ? cloud_ssrc(cs->scr.session_id) : BSDR_VIDEO_SSRC;
    cs->vid = bsdr_video_sender_new(&cs->vid_udp, NULL, BSDR_VIDEO_PT_DEFAULT, vssrc);
    if (!cs->vid) { bsdr_udp_close(&cs->vid_udp); return; }
    cs->last_sr = bsdr_now_ms();
    /* Comedia latch = a BURST of valid-RTP keepalives (matches the official host, which sends
     * 1-byte RTP packets on the video port so the Mediasoup PlainTransport latches our source
     * tuple). A burst — spread over ~N*5ms — gives a stale/previous-session tuple the best chance
     * to (re-)latch onto our new source port. This runs on every share start, i.e. on every
     * OFF->ON toggle (the stream is recreated each time), so it is also the re-latch.
     *
     * ROLLBACK: --cloud-latch-burst N tunes it (default 12); 3 = the previous behaviour,
     * 1 = a single latch, 0 = skip the burst entirely (the 1/s keepalive remains). */
    int latch_burst = (cs->app && cs->app->cloud_latch_burst >= 0) ? cs->app->cloud_latch_burst : 12;
    for (int i = 0; i < latch_burst; i++) { bsdr_video_send_keepalive(cs->vid); bsdr_sleep_ms(5); }
    if (latch_burst) BSDR_INFO("bsdr.cloud", "video: comedia latch burst = %d keepalives "
                               "(--cloud-latch-burst to tune/rollback)", latch_burst);

    /* DECOUPLED (--video-decoupled): open our OWN capture+encoder — a second nvenc session, more
     * CPU/GPU, but the relay can't perturb the LAN video. COUPLED (default): relay the single LAN
     * encoder's already-encoded access units (fed via the FIFO) — half the capture/encode cost and
     * the cloud automatically tracks the headset's bitrate/resolution. */
    bsdr_capture *cap = NULL;
    uint8_t *scratch = NULL; size_t scratch_cap = 0;
    if (cs->decoupled) {
        bsdr_capture_config cfg = {0};
        cfg.fps = 30;
        int rx=0,ry=0,rw=0,rh=0, qw=0,qh=0,qbr=0;
        if (cs->app) { bsdr_app_get_region(cs->app, &rx,&ry,&rw,&rh);
                       bsdr_app_get_quality(cs->app, &qw,&qh,&qbr); }
        cfg.x=rx; cfg.y=ry; cfg.width=rw; cfg.height=rh;
        cfg.out_width = qw>0?qw:0; cfg.out_height = qh>0?qh:0;
        cfg.bitrate = qbr>0 ? qbr : 2000000;
        cfg.cpu_only = cs->app && cs->app->cpu_only;
        cfg.use_vaapi = cs->app && cs->app->use_vaapi;
        cfg.use_kmsgrab = cs->app && cs->app->use_kmsgrab;
        /* 2D->3D: convert the decoupled cloud encode too, so internet viewers get the same SBS the
         * LAN headset does. The capture builds the transform from cfg.threed_*; it needs the CPU NV12
         * path. AI depth is downgraded to the fast heuristic here to avoid a second helper process. */
        int td_mode = 0, td_deep = 0, td_conv = 0, td_swap = 0, td_full = 0;
        if (cs->app) bsdr_app_get_threed(cs->app, &td_mode, &td_deep, &td_conv, &td_swap, &td_full, NULL, 0);
        if (td_mode != BSDR_3D_OFF) {
            cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0;
            if (cs->app && cs->app->cpu_only) cfg.encoder = "libx264";
            cfg.threed_mode = (td_mode == BSDR_3D_AI) ? BSDR_3D_FAST : td_mode;
            cfg.threed_deepness = td_deep; cfg.threed_convergence = td_conv;
            cfg.threed_swap = td_swap; cfg.threed_full = td_full;
        }
        cap = bsdr_capture_open(&cfg);
        if (!cap) {
            BSDR_ERROR("bsdr.cloud", "video: own capture/encode open failed (GPU busy?)");
            bsdr_video_sender_free(cs->vid); cs->vid = NULL;
            bsdr_udp_close(&cs->vid_udp);
            return;
        }
        const char *enc=NULL; bsdr_capture_info(cap, &cs->vid_w, &cs->vid_h, &enc);
        BSDR_INFO("bsdr.cloud", "video: OWN encode %dx%d via %s @ %dbps -> %s:%d (ssrc %u; decoupled)",
                  cs->vid_w, cs->vid_h, enc?enc:"?", cfg.bitrate, cs->scr.media_ip, cs->scr.video_port, vssrc);
    } else {
        BSDR_INFO("bsdr.cloud", "video: relaying the LAN encoder -> %s:%d (ssrc %u; coupled)",
                  cs->scr.media_ip, cs->scr.video_port, vssrc);
    }
    cs->video_ready = 1;
    long sent = 0; uint64_t last_log = bsdr_now_ms();
    while (!cs->stop) {
        /* every iteration (even when idle): keep the comedia tuple alive with a periodic
         * keepalive, like the official host. (The relay is receive-only — it never sends
         * RTCP/PLI back — so there's nothing to read on this socket.) */
        uint64_t now = bsdr_now_ms();
        if (now - cs->last_sr >= 1000) { bsdr_video_send_keepalive(cs->vid); cs->last_sr = now; }
        /* paused (share toggled off): keep the socket + keepalives alive but send no frames. */
        if (!cs->active) {
            if (!cs->decoupled) { bsdr_mutex_lock(cs->flock); cs->vq_head = cs->vq_tail; bsdr_mutex_unlock(cs->flock); }
            bsdr_sleep_ms(20); continue;
        }

        const uint8_t *au = NULL; size_t len = 0;
        if (cs->decoupled) {
            uint32_t ts;
            int r = bsdr_capture_frame(cap, &au, &len, &ts);
            if (r <= 0) { if (r < 0) break; bsdr_sleep_ms(2); }
            if (r <= 0) continue;
        } else {                                        /* coupled: dequeue a LAN-encoded AU */
            bsdr_mutex_lock(cs->flock);
            if (cs->vq_head != cs->vq_tail) {
                struct cloud_vframe *f = &cs->vq[cs->vq_head];
                if (f->len > scratch_cap) { uint8_t *nb = realloc(scratch, f->len); if (nb) { scratch = nb; scratch_cap = f->len; } }
                if (scratch && f->len <= scratch_cap) { memcpy(scratch, f->buf, f->len); len = f->len; au = scratch; }
                cs->vq_head = (cs->vq_head + 1) % BSDR_CLOUD_VQ;
            }
            bsdr_mutex_unlock(cs->flock);
            if (!au) { bsdr_sleep_ms(2); continue; }
        }
        bsdr_video_send_au_cloud(cs->vid, au, len, cs->vid_w, cs->vid_h);   /* [NAL][16B trailer] + pacing */
        sent++;
        if (now - last_log >= 3000) {
            BSDR_INFO("bsdr.cloud", "video: %ld frames relayed (%s)",
                      sent, cs->decoupled ? "own encode" : "coupled");
            last_log = now;
        }
    }
    cs->video_ready = 0;
    if (cap) bsdr_capture_close(cap);
    free(scratch);
    bsdr_video_sender_free(cs->vid); cs->vid = NULL;
    bsdr_udp_close(&cs->vid_udp);
    BSDR_INFO("bsdr.cloud", "video relay stopped (%ld frames)", sent);
}

/* ---- data/control channel: relay dataPort, RAW SCTP-over-UDP (usrsctp, port
 * 5000) — NOT DTLS. This is BigSoupCreateDataConnection: the SCTP association
 * registers our session with the relay (so it forwards our media), and the
 * DataChannel carries room data / input opcodes. Reversed from FUN_180085de0. ---- */
#ifdef BSDR_ENABLE_SCTP
struct cloud_data_ctx { bsdr_cloud_stream *cs; bsdr_injector *inj; long n_ev; };

/* a DataChannel message arrived from the relay (room data, or input opcodes) */
static void cloud_data_msg(const uint8_t *data, size_t len, void *user) {
    struct cloud_data_ctx *dc = (struct cloud_data_ctx *)user;
    if (len == 0) return;
    /* try to decode as input opcodes (mouse/keyboard); harmless if it's room data */
    bsdr_input_event evs[32];
    size_t ne = bsdr_decode_binary(data, len, evs, 32);
    for (size_t i = 0; i < ne; i++) { if (dc->inj) bsdr_injector_handle(dc->inj, &evs[i]); dc->n_ev++; }
    BSDR_DEBUG("bsdr.cloud", "data: %zuB DataChannel message (%zu events)", len, ne);
}

static void cloud_input_main(void *arg) {
    bsdr_cloud_stream *cs = (bsdr_cloud_stream *)arg;
    bsdr_udp udp;
    /* data source port: --cloud-src-port + 2, else sticky/ephemeral (see cloud_src_pick). */
    uint16_t dfixed = (cs->app && cs->app->cloud_src_port > 0) ? (uint16_t)(cs->app->cloud_src_port + 2) : 0;
    uint16_t dsrc = cloud_src_pick(cs->app, cs->scr.media_ip, 2, dfixed);
    if (!bsdr_udp_open(&udp, dsrc, cs->scr.media_ip, (uint16_t)cs->scr.data_port)) {
        BSDR_WARN("bsdr.cloud", "data: udp -> %s:%d failed", cs->scr.media_ip, cs->scr.data_port);
        return;
    }
    if (dsrc == 0) cloud_src_record(cs->app, cs->scr.media_ip, 2, bsdr_udp_local_port(&udp));
    struct cloud_data_ctx dc = { cs, bsdr_injector_create(1920, 1080), 0 };
    uint8_t buf[4096];

    /* The relay's DataChannel: RAW SCTP-over-UDP (RFC 6951) is PROVEN to associate with the relay
     * (live debug.log reached assoc=1), so try it FIRST. If it doesn't associate within 4s, fall
     * back to SCTP-over-DTLS (RFC 8261). --cloud-data raw|dtls forces one. The SCTP association
     * registers our session so the relay forwards our media. */
    /* Transport: RAW SCTP-over-UDP by default — confirmed from BigSoup.dll (usrsctp UDP
     * encapsulation, dgram_sctp_read/write; NO DTLS on the cloud data port). --cloud-data
     * dtls|both is kept only for experimentation. */
    const char *mode = (cs->app && cs->app->cloud_data_mode[0]) ? cs->app->cloud_data_mode : NULL;
    bool try_raw  = !mode || strcmp(mode, "raw") == 0 || strcmp(mode, "both") == 0;
    bool try_dtls = mode && (strcmp(mode, "dtls") == 0 || strcmp(mode, "both") == 0);

    /* ---- 1) raw SCTP-over-UDP (the real cloud data transport) ----
     * Reverse of the Quest's libBigSoup.so (BigData / data.cpp): the relay is a FULL SCTP peer —
     * it replies INIT-ACK / COOKIE-ACK / SACK and the association completes normally (proven in
     * full.pcapng). The host is RECEIVE-ONLY: it brings the association up and CONSUMES other peers'
     * avatar/identity/presence (FlatBuffers strings on PPID 51), but sends ZERO application bytes and
     * NO DCEP — mediasoup negotiates stream ids out-of-band via the room signaling. Sending a DCEP
     * DATA_CHANNEL_OPEN (what bsdrX used to do) is what crashed the Quest's data consumer, so we do
     * NOT open a channel here; we only associate + receive, exactly like the official host. */
    if (try_raw && !cs->stop) {
        /* On a re-share to the SAME relay tuple (sticky ports), the relay may still hold a stale
         * association from the previous share and ABORT our INIT (a collision). Its ABORT clears
         * that stale assoc, so recreating + retrying then associates cleanly. Retry a few times. */
        bsdr_sctp *sctp = NULL;
        bool associated = false;
        for (int attempt = 0; attempt < 5 && !cs->stop && !associated; attempt++) {
            sctp = bsdr_sctp_new_udp(&udp, true /*initiator*/, cloud_data_msg, &dc);
            if (!sctp || !bsdr_sctp_start(sctp, 5000)) { if (sctp) { bsdr_sctp_free(sctp); sctp = NULL; } break; }
            BSDR_INFO("bsdr.cloud", "data: raw SCTP INIT -> relay %s:%d%s (receive-only; no DCEP)",
                      cs->scr.media_ip, cs->scr.data_port, attempt ? " [retry]" : "");
            uint64_t last = bsdr_now_ms(), t0 = last;
            while (!cs->stop) {                                  /* wait for assoc or ABORT (~4s) */
                int n = bsdr_udp_recv(&udp, buf, sizeof(buf), 50);
                if (n > 0) bsdr_sctp_feed(sctp, buf, (size_t)n);
                uint64_t now = bsdr_now_ms();
                bsdr_sctp_handle_timers((uint32_t)(now - last)); last = now;
                if (bsdr_sctp_associated(sctp)) { associated = true; break; }
                if (bsdr_sctp_failed(sctp)) break;               /* relay ABORTed -> recreate + retry */
                if (now - t0 > 4000) break;                      /* timed out -> retry */
            }
            if (associated) break;
            bsdr_sctp_free(sctp); sctp = NULL;                   /* ABORT cleared the relay's stale assoc */
            if (!cs->stop) bsdr_sleep_ms(300);
        }
        if (sctp && associated) {
            BSDR_INFO("bsdr.cloud", "data: raw SCTP associated (session %s)", cs->scr.session_id);
            uint64_t last = bsdr_now_ms();
            while (!cs->stop) {                                  /* steady-state receive */
                int n = bsdr_udp_recv(&udp, buf, sizeof(buf), 50);
                if (n > 0) bsdr_sctp_feed(sctp, buf, (size_t)n);
                uint64_t now = bsdr_now_ms();
                bsdr_sctp_handle_timers((uint32_t)(now - last)); last = now;
            }
            bsdr_sctp_free(sctp);
            goto done;
        }
        if (sctp) bsdr_sctp_free(sctp);
        if (!cs->stop) BSDR_WARN("bsdr.cloud", "data: raw SCTP did not associate after retries");
    }

    /* ---- 2) SCTP-over-DTLS fallback (client then server; --cloud-dtls-role forces one) ---- */
    if (try_dtls && !cs->stop) {
        /* DTLS role selectable via --cloud-dtls-role client|server (default auto: client then server) */
        const char *role_env = (cs->app && cs->app->cloud_dtls_role[0]) ? cs->app->cloud_dtls_role : NULL;
        bsdr_dtls *dtls = NULL;
        for (int attempt = 0; attempt < 2 && !cs->stop; attempt++) {
            bsdr_dtls_role role;
            if (role_env) role = (strcmp(role_env, "server") == 0) ? BSDR_DTLS_SERVER : BSDR_DTLS_CLIENT;
            else          role = (attempt == 0) ? BSDR_DTLS_CLIENT : BSDR_DTLS_SERVER;
            dtls = bsdr_dtls_new(&udp, role);
            if (!dtls) break;
            if (role == BSDR_DTLS_SERVER) cloud_latch(&udp);  /* register our addr first */
            BSDR_INFO("bsdr.cloud", "data: DTLS %s handshake -> relay %s:%d",
                      role == BSDR_DTLS_SERVER ? "server" : "client", cs->scr.media_ip, cs->scr.data_port);
            if (bsdr_dtls_handshake(dtls, 6000, (volatile int *)&cs->stop)) break;   /* connected */
            BSDR_WARN("bsdr.cloud", "data: DTLS %s handshake failed",
                      role == BSDR_DTLS_SERVER ? "server" : "client");
            bsdr_dtls_free(dtls); dtls = NULL;
            if (role_env) break;
        }
        if (dtls) {
            bsdr_sctp *sctp = bsdr_sctp_new(dtls, true /*initiator*/, cloud_data_msg, &dc);
            if (sctp && bsdr_sctp_start(sctp, 5000)) {
                /* receive-only, no DCEP (see the raw path above) */
                BSDR_INFO("bsdr.cloud", "data: DTLS connected; SCTP DataChannel bringing up");
                uint64_t last = bsdr_now_ms();
                bool announced = false;
                while (!cs->stop) {
                    int n = bsdr_dtls_recv(dtls, buf, sizeof(buf), 50);
                    if (n > 0) bsdr_sctp_feed(sctp, buf, (size_t)n);
                    uint64_t now = bsdr_now_ms();
                    bsdr_sctp_handle_timers((uint32_t)(now - last)); last = now;
                    if (!announced && bsdr_sctp_channel_open(sctp)) {
                        announced = true;
                        BSDR_INFO("bsdr.cloud", "data: SCTP-over-DTLS DataChannel open (session %s)", cs->scr.session_id);
                    }
                }
            }
            if (sctp) bsdr_sctp_free(sctp);
            bsdr_dtls_free(dtls);
        } else {
            BSDR_WARN("bsdr.cloud", "data: DTLS handshake failed (both roles)");
        }
    }

done:
    if (dc.inj) bsdr_injector_destroy(dc.inj);
    bsdr_udp_close(&udp);
    BSDR_INFO("bsdr.cloud", "data stopped (%ld events)", dc.n_ev);
}
#else
static void cloud_input_main(void *arg) {
    bsdr_cloud_stream *cs = (bsdr_cloud_stream *)arg;
    BSDR_WARN("bsdr.cloud", "data channel needs an SCTP build (BSDR_ENABLE_SCTP); skipped");
    (void)cs;
}
#endif /* BSDR_ENABLE_SCTP */
#endif /* BSDR_HAVE_CAPTURE */

#ifdef BSDR_HAVE_AUDIO
struct cloud_audio_shared { bsdr_cloud_stream *cs; bsdr_udp *udp; bsdr_audio_devices *dev; };
/* desktop audio -> relay audioPort. PLAIN RTP (Opus stereo, pt 100). */
static void cloud_audio_send_main(void *arg) {
    struct cloud_audio_shared *sh = (struct cloud_audio_shared *)arg;
    /* SSRC = djb2(userSessionId), same registration model as video (separate Mediasoup
     * producer on audioPort). Derived by analogy with the video path. */
    uint32_t assrc = sh->cs->scr.session_id[0] ? cloud_ssrc(sh->cs->scr.session_id) : BSDR_AUDIO_SSRC;
    bsdr_audio_sender *tx = bsdr_audio_sender_new(sh->udp, NULL /*plain*/, BSDR_AUDIO_PT_DEFAULT,
                                                  assrc, BSDR_AUDIO_CHANNELS,
                                                  BSDR_AUDIO_DESKTOP_BPS);
    if (!tx) { BSDR_WARN("bsdr.cloud", "audio-out: sender init failed"); return; }
    bsdr_audio_sender_enable_cloud_trailer(tx);   /* [u32 ssrc LE][u32 frame_id LE] after Opus, no XOR */
    /* Cloud uses 10ms Opus frames (ts += 480), matching the official host (full.pcapng :50039
     * shows pt 100, ssrc djb2, ts step 480, marker 0) — NOT the 20ms LAN frame. */
    enum { CLOUD_OPUS_FRAME = 480 };
    int16_t pcm[CLOUD_OPUS_FRAME * BSDR_AUDIO_CHANNELS];

#ifdef BSDR_HAVE_CAPTURE
    /* File streaming: the room hears the FILE's own audio, sharing the media-bar volume/pause/seek
     * with the LAN side via bsdr_app. A separate fileaudio instance keeps this decoupled from the
     * LAN one (like desktop's two independent captures), so both stay paced to their own PTS. */
    bsdr_app *app = sh->cs->app;
    if (app && strcmp(app->source, "file") == 0 && app->source_path[0]) {
        int pl_is = bsdr_path_is_playlist(app->source_path);
        int pl_idx = 0;
        char curpath[512];
        if (bsdr_playlist_entry(app->source_path, pl_idx, curpath, sizeof curpath) == 0) {
            bsdr_audio_sender_free(tx); return; }
        bsdr_fileaudio *fa = bsdr_fileaudio_open(curpath, 48000, BSDR_AUDIO_CHANNELS, pl_is ? false : true);
        if (!fa) { BSDR_INFO("bsdr.cloud", "audio: %s has no audio track", curpath);
                   bsdr_audio_sender_free(tx); return; }
        BSDR_INFO("bsdr.cloud", "audio: file %s -> relay (plain RTP pt %d, ssrc %u)",
                  curpath, BSDR_AUDIO_PT_DEFAULT, assrc);
        unsigned my_seek = app->file_seek_gen;
        while (!sh->cs->stop) {
            bsdr_fileaudio_set_paused(fa, app->file_paused);
            if (app->file_seek_gen != my_seek) { bsdr_fileaudio_seek(fa, app->file_seek_frac); my_seek = app->file_seek_gen; }
            bsdr_audio_sender_set_gain(tx, app->file_volume);   /* media-bar volume */
            int n = bsdr_fileaudio_read(fa, pcm, CLOUD_OPUS_FRAME);
            if (n == CLOUD_OPUS_FRAME) { if (sh->cs->active) bsdr_audio_send_pcm(tx, pcm, CLOUD_OPUS_FRAME); }
            else if (n < 0 && pl_is) {   /* clip ended -> next playlist entry */
                bsdr_fileaudio_close(fa); fa = NULL;
                for (int t = 0; t < 64 && !fa; t++) {
                    if (bsdr_playlist_entry(app->source_path, ++pl_idx, curpath, sizeof curpath) == 0) break;
                    fa = bsdr_fileaudio_open(curpath, 48000, BSDR_AUDIO_CHANNELS, false);
                }
                if (!fa) { bsdr_sleep_ms(20); if (bsdr_playlist_entry(app->source_path, pl_idx, curpath, sizeof curpath)) continue; break; }
            }
            else bsdr_sleep_ms(3);   /* paused / underrun */
        }
        if (fa) bsdr_fileaudio_close(fa);
        bsdr_audio_sender_free(tx);
        BSDR_INFO("bsdr.cloud", "audio-out (file) stopped");
        return;
    }
#endif

    bsdr_pa *cap = bsdr_pa_record_open(sh->dev->monitor_source, BSDR_AUDIO_CHANNELS);
    if (!cap) { BSDR_WARN("bsdr.cloud", "audio-out: capture init failed"); bsdr_audio_sender_free(tx); return; }
    BSDR_INFO("bsdr.cloud", "audio: desktop -> relay (plain RTP pt %d, +8B trailer ssrc %u)",
              BSDR_AUDIO_PT_DEFAULT, assrc);
    while (!sh->cs->stop) {
        /* always drain the capture (avoids a burst on resume); send only while active. */
        if (bsdr_pa_read(cap, pcm, CLOUD_OPUS_FRAME) == CLOUD_OPUS_FRAME && sh->cs->active)
            bsdr_audio_send_pcm(tx, pcm, CLOUD_OPUS_FRAME);
    }
    bsdr_audio_sender_free(tx); bsdr_pa_close(cap);
    BSDR_INFO("bsdr.cloud", "audio-out stopped");
}
/* relay -> virtual mic. PLAIN RTP (pt 100): incoming room mic audio (the Quest /
 * other users), decoded and played into the BSRD virtual mic. */
struct mic_cb_ctx { bsdr_audio_player *play; bsdr_app *app; };
static void mic_pcm_cb(const int16_t *pcm, int frames, int channels, void *user) {
    struct mic_cb_ctx *c = (struct mic_cb_ctx *)user;
    bsdr_audio_player_push(c->play, pcm, frames);
    /* Owner-mic cloud fallback: also feed the compctl voice pipeline. While a command is captured
     * the duck (below) has already soloed the loudest room stream, so this is the owner's voice. */
    if (c->app && c->app->cloud_mic_fallback && c->app->room_pcm_cb)
        c->app->room_pcm_cb(c->app->room_pcm_user, pcm, frames, channels);
}
static void cloud_mic_main(void *arg) {
    struct cloud_audio_shared *sh = (struct cloud_audio_shared *)arg;
    bsdr_audio_player *play = bsdr_audio_player_new(sh->dev->mic_sink, BSDR_AUDIO_CHANNELS);
    struct mic_cb_ctx ctx = { play, sh->cs->app };
    bsdr_audio_recv *rx = bsdr_audio_recv_new(NULL /*plain*/, BSDR_AUDIO_PT_DEFAULT, BSDR_AUDIO_SSRC,
                                              BSDR_AUDIO_CHANNELS, mic_pcm_cb, &ctx);
    if (!play || !rx) {
        if (play) bsdr_audio_player_free(play);
        if (rx) bsdr_audio_recv_free(rx);
        return;
    }
    uint8_t buf[2048];
    uint64_t next_play = bsdr_now_ms();   /* 20 ms playout clock for the jitter buffer */
    while (!sh->cs->stop) {
        /* Arm the voice-activity duck only while a command is captured over the cloud fallback. */
        bsdr_app *app = sh->cs->app;
        bsdr_audio_recv_set_duck(rx, (app && app->cloud_mic_fallback && app->cloud_mic_duck) ? 1 : 0);
        int n = bsdr_udp_recv(sh->udp, buf, sizeof(buf), 5);   /* short timeout so playout stays on time */
        if (n > 0) bsdr_audio_recv_feed(rx, buf, n);
        uint64_t now = bsdr_now_ms();
        while ((int64_t)(now - next_play) >= 0) {
            bsdr_audio_recv_playout(rx);
            next_play += 20;
        }
    }
    bsdr_audio_recv_free(rx); bsdr_audio_player_free(play);
}
/* audio channel owner: plain RTP, comedia latch, then send + mic threads. */
static void cloud_audio_main(void *arg) {
    bsdr_cloud_stream *cs = (bsdr_cloud_stream *)arg;
    bsdr_udp udp;
    /* audio source port: --cloud-src-port + 1, else sticky/ephemeral (see cloud_src_pick). */
    uint16_t afixed = (cs->app && cs->app->cloud_src_port > 0) ? (uint16_t)(cs->app->cloud_src_port + 1) : 0;
    uint16_t asrc = cloud_src_pick(cs->app, cs->scr.media_ip, 1, afixed);
    if (!bsdr_udp_open(&udp, asrc, cs->scr.media_ip, (uint16_t)cs->scr.audio_port)) {
        BSDR_WARN("bsdr.cloud", "audio: udp -> %s:%d failed", cs->scr.media_ip, cs->scr.audio_port);
        return;
    }
    if (asrc == 0) cloud_src_record(cs->app, cs->scr.media_ip, 1, bsdr_udp_local_port(&udp));
    cloud_latch(&udp);
    /* The virtual audio devices are owned by the LAN path (lan_live_main) and must
     * stay up independently of cloud — do NOT create/destroy them here (devices_create
     * runs pactl_unload_stale, which would tear down the LAN desktop-audio devices
     * mid-session). Reference the stable device names the LAN path established. */
    bsdr_audio_devices dev;
    memset(&dev, 0, sizeof dev);
    dev.speaker_module = dev.mic_sink_module = dev.mic_source_module = -1;
    strcpy(dev.monitor_source, "bsdr_speaker.monitor");
    strcpy(dev.mic_sink, "bsdr_micsink");
    struct cloud_audio_shared sh = { cs, &udp, &dev };
    bsdr_thread *st = bsdr_thread_start(cloud_audio_send_main, &sh);
    bsdr_thread *mt = bsdr_thread_start(cloud_mic_main, &sh);
    while (!cs->stop) bsdr_sleep_ms(100);
    if (st) bsdr_thread_join(st);
    if (mt) bsdr_thread_join(mt);
    bsdr_udp_close(&udp);
}
#endif /* BSDR_HAVE_AUDIO */


bsdr_cloud_stream *bsdr_cloud_stream_start(const bsdr_cloud_screen *scr,
                                           bsdr_app *app, bool audio) {
#ifndef BSDR_HAVE_CAPTURE
    (void)scr; (void)app; (void)audio;
    BSDR_WARN("bsdr.cloud", "relay streaming needs a capture build; not started");
    return NULL;
#else
    if (!scr || !scr->found || !scr->media_ip[0] || scr->video_port <= 0) {
        BSDR_WARN("bsdr.cloud", "no relay screen to stream to");
        return NULL;
    }
    bsdr_cloud_stream *cs = calloc(1, sizeof(*cs));
    if (!cs) return NULL;
    cs->app = app; cs->scr = *scr; cs->audio = audio;
    cs->active = 1;
    cs->decoupled = app && app->video_decoupled;
    cs->flock = bsdr_mutex_new();        /* FIFO mutex for the coupled (LAN-fed) video path */
    if (!cs->flock) cs->decoupled = true; /* coupled mode needs it; fall back to own-capture if alloc fails */
    cs->vid_w = 1280; cs->vid_h = 720;   /* sane default until the first fed frame / own capture */
    if (app && app->cloud_no_video)
        BSDR_INFO("bsdr.cloud", "cloud video OFF (--no-cloud-video; diagnostic)");
    else
        cs->vthr = bsdr_thread_start(cloud_video_main, cs);
    /* SCTP data channel: now receive-only (no DCEP — that's what used to crash the Quest), it brings
     * up the association exactly like the official host. mediasoup may withhold a peer's producers
     * from consumers until ALL its transports (incl. data) are connected, so being a fully-joined
     * peer is likely required for OTHER viewers to actually receive our screen. ON by default;
     * --cloud-data off disables (raw|dtls|both pick the transport). */
    const char *dmode = (app && app->cloud_data_mode[0]) ? app->cloud_data_mode : "raw";
    if (scr->data_port > 0 && strcmp(dmode, "off") != 0)
        cs->ithr = bsdr_thread_start(cloud_input_main, cs);
    else
        BSDR_INFO("bsdr.cloud", "data channel OFF (--cloud-data off)");
#ifdef BSDR_HAVE_AUDIO
    /* Cloud AUDIO is ENCRYPTED on the wire too (same as video) — sending plain Opus (pt 100)
     * Cloud audio is plain Opus RTP (pt 100, djb2 SSRC, ts+=480) + the 8-byte BigSoup trailer
     * ([u32 ssrc LE][u32 frame_id LE], no XOR) so the Quest reads our SSRC. ON by default. */
    if (audio && scr->audio_port > 0 && !(app && app->cloud_no_audio))
        cs->athr = bsdr_thread_start(cloud_audio_main, cs);
    else if (audio)
        BSDR_INFO("bsdr.cloud", "cloud audio OFF (--no-cloud-audio)");
#endif
    BSDR_INFO("bsdr.cloud", "relay streaming started -> %s (v=%d a=%d d=%d)",
              scr->media_ip, scr->video_port, scr->audio_port, scr->data_port);
    return cs;
#endif
}

/* Pause/resume WITHOUT tearing down: keep the sockets bound + the SCTP association up (like the
 * official client, which never closes its connections on toggle — SetInternetStreamingEnabled just
 * flips a flag). Avoids the re-INIT collision + comedia re-latch that broke streaming on restart. */
void bsdr_cloud_stream_set_active(bsdr_cloud_stream *cs, int active) {
    if (!cs || cs->active == active) return;
    cs->active = active;
    BSDR_INFO("bsdr.cloud", "relay streaming %s (connection kept alive)", active ? "resumed" : "paused");
}
int bsdr_cloud_stream_active(bsdr_cloud_stream *cs) { return cs ? cs->active : 0; }

/* Same relay tuple as a running stream? (so a re-share can just resume instead of restart.) */
bool bsdr_cloud_stream_matches(bsdr_cloud_stream *cs, const bsdr_cloud_screen *scr) {
    return cs && scr && cs->scr.video_port == scr->video_port &&
           strcmp(cs->scr.media_ip, scr->media_ip) == 0 &&
           strcmp(cs->scr.session_id, scr->session_id) == 0;
}

void bsdr_cloud_stream_stop(bsdr_cloud_stream *cs) {
    if (!cs) return;
    cs->stop = 1;
    if (cs->vthr) bsdr_thread_join(cs->vthr);
    if (cs->athr) bsdr_thread_join(cs->athr);
    if (cs->mthr) bsdr_thread_join(cs->mthr);
    if (cs->ithr) bsdr_thread_join(cs->ithr);
    for (int i = 0; i < BSDR_CLOUD_VQ; i++) free(cs->vq[i].buf);   /* FIFO frame buffers */
    if (cs->flock) bsdr_mutex_free(cs->flock);
    BSDR_INFO("bsdr.cloud", "relay streaming stopped");
    free(cs);
}
