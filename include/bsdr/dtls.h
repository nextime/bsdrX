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
/* DTLS-SRTP over the no-ICE UDP transport (OpenSSL).
 *
 * Bigscreen opens a raw DTLS handshake to the headset on UDP 45002: self-signed
 * cert, no peer-fingerprint verification (none is exchanged), SRTP profiles
 * advertised. After the handshake, DTLS application data carries SCTP (RFC 8261)
 * — handled by the optional sctp layer. */
#ifndef BSDR_DTLS_H
#define BSDR_DTLS_H

#include "bsdr/udp_transport.h"
#include <stddef.h>

typedef enum { BSDR_DTLS_CLIENT, BSDR_DTLS_SERVER } bsdr_dtls_role;

typedef struct bsdr_dtls bsdr_dtls;

bsdr_dtls *bsdr_dtls_new(bsdr_udp *udp, bsdr_dtls_role role);

/* Optional pre-DTLS UDP hello beacon: while handshaking, resend `buf` every
 * `interval_ms` (the Bigscreen LAN 0x01234567 hello the headset waits for). */
void bsdr_dtls_set_hello(bsdr_dtls *d, const void *buf, size_t len, int interval_ms);

/* Drive the handshake to completion (true) or timeout/failure (false).
 * If `cancel` is non-NULL and becomes nonzero, aborts early. */
bool bsdr_dtls_handshake(bsdr_dtls *d, int timeout_ms, volatile int *cancel);

/* Application data (post-handshake). recv returns #bytes, 0 on timeout, <0 err. */
int bsdr_dtls_send(bsdr_dtls *d, const void *buf, size_t len);
int bsdr_dtls_recv(bsdr_dtls *d, void *buf, size_t len, int timeout_ms);

/* Feed one already-received DTLS datagram and deliver each decrypted application
 * record to `cb` (used by a demuxing pump that reads UDP itself and routes
 * DTLS vs SRTP by first byte). */
typedef void (*bsdr_dtls_appdata_cb)(const uint8_t *data, int len, void *user);
void bsdr_dtls_process(bsdr_dtls *d, const uint8_t *dgram, int dlen,
                       bsdr_dtls_appdata_cb cb, void *user);

/* Peer cert subject + negotiated SRTP profile name (diagnostics). */
void bsdr_dtls_peer_info(bsdr_dtls *d, char *subject, size_t slen,
                         char *srtp, size_t plen);

/* DTLS-SRTP keying material (RFC 5764) for the media (video/audio) channel. */
typedef enum {
    BSDR_SRTP_NONE = 0,
    BSDR_SRTP_AES128_CM_SHA1_80,
    BSDR_SRTP_AEAD_AES_128_GCM
} bsdr_srtp_profile;

typedef struct {
    bsdr_srtp_profile profile;
    uint8_t send_master[32];   /* our key||salt (we are the RTP sender) */
    size_t  send_master_len;
    uint8_t recv_master[32];   /* peer key||salt */
    size_t  recv_master_len;
} bsdr_srtp_keys;

/* Export SRTP keys from the completed handshake. False if no SRTP profile or an
 * unsupported one was negotiated. */
bool bsdr_dtls_export_srtp(bsdr_dtls *d, bsdr_srtp_keys *out);

void bsdr_dtls_free(bsdr_dtls *d);

#endif /* BSDR_DTLS_H */
