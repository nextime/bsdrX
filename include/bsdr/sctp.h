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
/* SCTP-over-DTLS WebRTC DataChannel (usrsctp) — built with BSDR_ENABLE_SCTP.
 *
 * The headset carries input over a WebRTC DataChannel = SCTP/DTLS (RFC 8261) +
 * DCEP channel setup (RFC 8832), binary messages on SCTP PPID 53. This module
 * runs usrsctp in AF_CONN mode with its output encapsulated in our DTLS link,
 * drives it single-threaded (usrsctp_init_nothreads + handle_timers), answers
 * DATA_CHANNEL_OPEN with DATA_CHANNEL_ACK, and delivers binary messages to a
 * callback. It can also open a channel + send (used by the loopback test that
 * stands in for the headset).
 */
#ifndef BSDR_SCTP_H
#define BSDR_SCTP_H

#include "bsdr/dtls.h"
#include "bsdr/udp_transport.h"
#include <stddef.h>
#include <stdint.h>

typedef void (*bsdr_dc_msg_cb)(const uint8_t *data, size_t len, void *user);

typedef struct bsdr_sctp bsdr_sctp;

bsdr_sctp *bsdr_sctp_new(bsdr_dtls *dtls, bool initiator,
                         bsdr_dc_msg_cb cb, void *user);
/* Raw SCTP-over-UDP (RFC 6951), NO DTLS — for the Bigscreen cloud relay data
 * channel (usrsctp AF_CONN, real CRC32c, encapsulated in `udp`). `udp` is
 * borrowed and must already target the relay's dataPort. */
bsdr_sctp *bsdr_sctp_new_udp(bsdr_udp *udp, bool initiator,
                             bsdr_dc_msg_cb cb, void *user);
/* Create the association: initiator connects, responder listens/accepts. */
bool bsdr_sctp_start(bsdr_sctp *s, uint16_t port);
/* Feed one decrypted DTLS application datagram (SCTP packet) into usrsctp. */
void bsdr_sctp_feed(bsdr_sctp *s, const uint8_t *data, size_t len);
/* Drive SCTP timers (retransmits etc.); call from the pump loop. */
void bsdr_sctp_handle_timers(uint32_t elapsed_ms);

/* Open a DataChannel (DCEP) — for the opener/headset side. */
bool bsdr_sctp_open_channel(bsdr_sctp *s, const char *label);
/* True once a DataChannel is established (open seen + acked, or we acked one). */
bool bsdr_sctp_channel_open(bsdr_sctp *s);
/* True once the SCTP association itself is up (COMM_UP) — the relay registers our session on
 * this, even when it never completes the DCEP DataChannel handshake. */
bool bsdr_sctp_associated(bsdr_sctp *s);
/* True once the relay ABORTed our INIT or the association was lost — recreate + retry. */
bool bsdr_sctp_failed(bsdr_sctp *s);
/* Send a binary DataChannel message (PPID 53). */
int bsdr_sctp_send(bsdr_sctp *s, const uint8_t *data, size_t len);
/* Send a room broadcast message on SCTP stream 1 (BigData::Broadcast), PPID 53, WITHOUT DCEP —
 * the bot's avatar/pose FlatBuffer for the room data plane. Matches bsandroid bsa_media_send_data. */
int bsdr_sctp_send_room(bsdr_sctp *s, const uint8_t *data, size_t len);

void bsdr_sctp_free(bsdr_sctp *s);

#endif /* BSDR_SCTP_H */
