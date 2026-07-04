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
/* End-to-end WebRTC DataChannel test over the no-ICE DTLS link (usrsctp).
 * Requires BSDR_ENABLE_SCTP. No headset needed.
 *
 * Two endpoints over loopback UDP: the DTLS handshakes run concurrently (two
 * threads); then a single thread cooperatively drives both SCTP stacks (usrsctp
 * global timers must be driven from one thread). The "headset" opens a DCEP
 * DataChannel and sends a binary input frame; the "agent" receives it on the
 * channel and we decode it — exercising SCTP-over-DTLS + DCEP + PPID 53.
 */
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr/protocol.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/sctp.h"
#include "bsdr/input_decode.h"

#include <stdio.h>
#include <string.h>

#define PORT_AGENT   45112
#define PORT_HEADSET 45113

static volatile int g_got = 0;
static uint8_t g_frame[64];
static size_t g_frame_len = 0;

static void agent_cb(const uint8_t *data, size_t len, void *user) {
    (void)user;
    if (!g_got) {
        g_frame_len = len < sizeof(g_frame) ? len : sizeof(g_frame);
        memcpy(g_frame, data, g_frame_len);
        g_got = 1;
    }
}

/* the headset's DTLS handshake runs in its own thread (concurrent with agent) */
struct hs { bsdr_dtls *d; volatile int ok; };
static void headset_handshake(void *arg) {
    struct hs *h = (struct hs *)arg;
    h->ok = bsdr_dtls_handshake(h->d, 8000, NULL) ? 1 : 0;
}

static void put_i32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(void) {
    int failures = 0;
    bsdr_log_set_level(BSDR_LOG_WARN);
    if (!bsdr_platform_init()) { printf("platform init failed\n"); return 1; }

    bsdr_udp ua, uh;
    bsdr_udp_open(&ua, PORT_AGENT, "127.0.0.1", PORT_HEADSET);   /* agent: initiator */
    bsdr_udp_open(&uh, PORT_HEADSET, NULL, 0);                    /* headset: responder */
    bsdr_dtls *da = bsdr_dtls_new(&ua, BSDR_DTLS_CLIENT);
    bsdr_dtls *dh = bsdr_dtls_new(&uh, BSDR_DTLS_SERVER);

    /* concurrent DTLS handshake */
    struct hs h = { dh, 0 };
    bsdr_thread *t = bsdr_thread_start(headset_handshake, &h);
    int aok = bsdr_dtls_handshake(da, 8000, NULL) ? 1 : 0;
    bsdr_thread_join(t);
    if (aok && h.ok) printf("PASS dtls_handshake (both ends)\n");
    else { printf("FAIL dtls_handshake (agent=%d headset=%d)\n", aok, h.ok); failures++; }

    /* SCTP phase, single-threaded (usrsctp global timers) */
    bsdr_sctp *sa = bsdr_sctp_new(da, true,  agent_cb, NULL);    /* agent: SCTP client */
    bsdr_sctp *sh = bsdr_sctp_new(dh, false, NULL, NULL);        /* headset: SCTP server */
    bsdr_sctp_start(sa, BSDR_DEFAULT_SCTP_PORT);
    bsdr_sctp_start(sh, BSDR_DEFAULT_SCTP_PORT);
    bsdr_sctp_open_channel(sh, "input");   /* deferred until associated */

    uint8_t frame[9];
    frame[0] = BSDR_MSG_MOVE_REL;
    put_i32(frame + 1, 5);
    put_i32(frame + 5, -3);

    uint64_t last = bsdr_now_ms(), t0 = last;
    int sent = 0, chan_logged = 0;
    uint8_t buf[2048];
    while (!g_got && bsdr_now_ms() - t0 < 8000) {
        int na = bsdr_dtls_recv(da, buf, sizeof(buf), 20);
        if (na > 0) bsdr_sctp_feed(sa, buf, (size_t)na);
        int nh = bsdr_dtls_recv(dh, buf, sizeof(buf), 20);
        if (nh > 0) bsdr_sctp_feed(sh, buf, (size_t)nh);

        uint64_t now = bsdr_now_ms();
        bsdr_sctp_handle_timers((uint32_t)(now - last));
        last = now;

        if (bsdr_sctp_channel_open(sh)) {
            if (!chan_logged) { printf("PASS datachannel_open (DCEP)\n"); chan_logged = 1; }
            if (!sent) { bsdr_sctp_send(sh, frame, sizeof(frame)); sent = 1; }
        }
    }
    if (!chan_logged) { printf("FAIL datachannel_open\n"); failures++; }

    bsdr_input_event ev[4];
    size_t ne = g_got ? bsdr_decode_binary(g_frame, g_frame_len, ev, 4) : 0;
    if (ne == 1 && ev[0].kind == BSDR_EV_MOVE_REL &&
        ev[0].u.move_rel.dx == 5 && ev[0].u.move_rel.dy == -3)
        printf("PASS datachannel_binary_message (dx=5 dy=-3 over PPID 53)\n");
    else { printf("FAIL datachannel_binary_message (got=%d ne=%zu)\n", g_got, ne); failures++; }

    bsdr_sctp_free(sa);
    bsdr_sctp_free(sh);
    bsdr_dtls_free(da);
    bsdr_dtls_free(dh);
    bsdr_udp_close(&ua);
    bsdr_udp_close(&uh);
    bsdr_platform_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nOK - SCTP DataChannel round-trip passed\n",
           failures);
    return failures ? 1 : 0;
}
