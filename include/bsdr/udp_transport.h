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
/* Plain-UDP transport (no ICE) — the base for the DTLS handshake.
 *
 * Bigscreen does no ICE/STUN: the PC opens a raw DTLS handshake straight to the
 * headset on UDP 45002 (see the spec). This wraps a UDP socket with a learned
 * remote peer, blocking recv with a timeout, and a datagram classifier for
 * diagnostics. */
#ifndef BSDR_UDP_TRANSPORT_H
#define BSDR_UDP_TRANSPORT_H

#include "bsdr/platform.h"
#include <stddef.h>

typedef struct {
    bsdr_socket_t sock;
    struct sockaddr_in remote;
    bool have_remote;
} bsdr_udp;

/* Bind on local_port. If remote_ip != NULL we're the initiator and send there;
 * otherwise the remote is learned from the first datagram. */
bool bsdr_udp_open(bsdr_udp *u, uint16_t local_port,
                   const char *remote_ip, uint16_t remote_port);

int bsdr_udp_send(bsdr_udp *u, const void *buf, size_t len);
/* Batch-send `count` datagrams (each iov[i]) to the remote via sendmmsg (one syscall per 64). */
struct iovec;
int bsdr_udp_send_batch(bsdr_udp *u, const struct iovec *iov, int count);
/* Blocking recv with timeout (ms). Returns #bytes, 0 on timeout, <0 on error. */
int bsdr_udp_recv(bsdr_udp *u, void *buf, size_t len, int timeout_ms);

void bsdr_udp_close(bsdr_udp *u);

/* Actual local port the socket is bound to (host order), e.g. to read back an
 * ephemeral port the OS assigned. Returns 0 on error. */
uint16_t bsdr_udp_local_port(bsdr_udp *u);

/* First-byte classifier for raw datagrams (diagnostics). */
const char *bsdr_udp_classify(const void *data, size_t len);

#endif /* BSDR_UDP_TRANSPORT_H */
