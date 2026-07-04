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
/* End-to-end test of the no-ICE DTLS path, no headset required.
 *
 * Two endpoints handshake over loopback UDP (no ICE); the "headset" side then
 * sends an input frame as DTLS app-data which the "agent" side receives and
 * decodes. Proves UDP -> DTLS -> decode locally. (SCTP framing is the
 * BSDR_ENABLE_SCTP build; here the raw app-data path stands in for it.)
 */
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr/protocol.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/input_decode.h"

#include <stdio.h>
#include <string.h>

#define PORT_AGENT   45102
#define PORT_HEADSET 45103

typedef struct {
    int handshake_ok;
    int sent;
} headset_state;

static void put_i32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* "headset": DTLS server on PORT_HEADSET, sends one rel-move frame. */
static void headset_thread(void *arg) {
    headset_state *st = (headset_state *)arg;
    bsdr_udp u;
    if (!bsdr_udp_open(&u, PORT_HEADSET, NULL, 0)) return;
    bsdr_dtls *d = bsdr_dtls_new(&u, BSDR_DTLS_SERVER);
    if (bsdr_dtls_handshake(d, 8000, NULL)) {
        st->handshake_ok = 1;
        uint8_t frame[9];
        frame[0] = BSDR_MSG_MOVE_REL;
        put_i32(frame + 1, 5);
        put_i32(frame + 5, -3);
        bsdr_dtls_send(d, frame, sizeof(frame));
        st->sent = 1;
        bsdr_sleep_ms(500);   /* keep the link alive while the client reads */
    }
    bsdr_dtls_free(d);
    bsdr_udp_close(&u);
}

int main(void) {
    int failures = 0;
    bsdr_log_set_level(BSDR_LOG_WARN);
    if (!bsdr_platform_init()) { printf("platform init failed\n"); return 1; }

    headset_state st = { 0, 0 };
    bsdr_thread *t = bsdr_thread_start(headset_thread, &st);
    bsdr_sleep_ms(100);   /* let the responder bind first */

    bsdr_udp u;
    bsdr_udp_open(&u, PORT_AGENT, "127.0.0.1", PORT_HEADSET);
    bsdr_dtls *d = bsdr_dtls_new(&u, BSDR_DTLS_CLIENT);

    if (bsdr_dtls_handshake(d, 8000, NULL)) printf("PASS dtls_handshake (no-ICE, loopback)\n");
    else { printf("FAIL dtls_handshake\n"); failures++; }

    uint8_t buf[64];
    int n = bsdr_dtls_recv(d, buf, sizeof(buf), 4000);
    bsdr_input_event ev[4];
    size_t ne = (n > 0) ? bsdr_decode_binary(buf, (size_t)n, ev, 4) : 0;
    if (ne == 1 && ev[0].kind == BSDR_EV_MOVE_REL &&
        ev[0].u.move_rel.dx == 5 && ev[0].u.move_rel.dy == -3)
        printf("PASS datachannel_frame_decoded (dx=5 dy=-3)\n");
    else { printf("FAIL datachannel_frame_decoded (n=%d ne=%zu)\n", n, ne); failures++; }

    bsdr_dtls_free(d);
    bsdr_udp_close(&u);
    bsdr_thread_join(t);
    if (!st.handshake_ok) { printf("FAIL headset_handshake\n"); failures++; }

    bsdr_platform_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nOK - no-ICE DTLS transport round-trip passed\n",
           failures);
    return failures ? 1 : 0;
}
