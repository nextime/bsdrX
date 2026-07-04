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
/* Active DTLS probe — point it at a real Quest and watch the handshake.
 *
 * Bigscreen does no ICE: the PC opens a raw DTLS handshake to the headset on UDP
 * 45002. This runs exactly that with verbose datagram logging, to confirm the
 * Quest answers, the DTLS role, and that nothing non-DTLS arrives.
 *
 *   dtls_probe <quest-ip> [port] [--responder] [--timeout SEC]
 */
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr/protocol.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: dtls_probe <ip> [port] [--responder] [--timeout SEC]\n");
        return 2;
    }
    const char *ip = argv[1];
    uint16_t port = BSDR_REMOTE_DESKTOP_PORT;
    bool responder = false;
    int timeout_ms = 15000;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--responder") == 0) responder = true;
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_ms = atoi(argv[++i]) * 1000;
        else port = (uint16_t)atoi(argv[i]);
    }

    bsdr_log_set_level(BSDR_LOG_DEBUG);
    if (!bsdr_platform_init()) { fprintf(stderr, "platform init failed\n"); return 2; }

    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, BSDR_REMOTE_DESKTOP_PORT,
                       responder ? NULL : ip, port))
        return 2;

    bsdr_dtls *d = bsdr_dtls_new(&udp, responder ? BSDR_DTLS_SERVER : BSDR_DTLS_CLIENT);
    if (!d) { fprintf(stderr, "dtls init failed\n"); return 2; }

    BSDR_INFO("dtls_probe", "probing %s:%u as DTLS %s (timeout %ds) ...",
              ip, port, responder ? "server" : "client", timeout_ms / 1000);

    int rc;
    if (bsdr_dtls_handshake(d, timeout_ms, NULL)) {
        char subj[256], srtp[64];
        bsdr_dtls_peer_info(d, subj, sizeof(subj), srtp, sizeof(srtp));
        BSDR_INFO("dtls_probe", "SUCCESS: DTLS connected. srtp=%s peer=%s",
                  srtp[0] ? srtp : "?", subj[0] ? subj : "(none)");
        rc = 0;
    } else {
        BSDR_ERROR("dtls_probe", "no DTLS handshake (see rx logs: did anything "
                   "arrive? wrong role? try --responder)");
        rc = 1;
    }
    bsdr_dtls_free(d);
    bsdr_udp_close(&udp);
    bsdr_platform_cleanup();
    return rc;
}
