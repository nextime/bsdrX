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
/* H.264 video sender: RTP packetize (RFC 6184) + SRTP protect (libsrtp2) + send
 * on the muxed UDP/DTLS port. Keyed from the DTLS-SRTP handshake. Built with
 * BSDR_ENABLE_VIDEO. */
#ifndef BSDR_VIDEO_H
#define BSDR_VIDEO_H

#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/srtp_util.h"   /* bsdr_srtp_global_init/shutdown, session_create */
#include <stddef.h>
#include <stdint.h>

typedef struct bsdr_video_sender bsdr_video_sender;

/* `udp` is borrowed (the same socket the DTLS link uses; SRTP is multiplexed by
 * first byte). payload_type/ssrc default to 96 / a fixed value if 0. */
bsdr_video_sender *bsdr_video_sender_new(bsdr_udp *udp,
                                         const bsdr_srtp_keys *keys,
                                         uint8_t payload_type, uint32_t ssrc);

/* Send one Annex-B access unit (start-code separated NAL units). rtp_ts is the
 * 90 kHz timestamp for this frame. Returns 0 on success. */
int bsdr_video_send_access_unit(bsdr_video_sender *v,
                                const uint8_t *annexb, size_t len,
                                uint32_t rtp_ts);

/* Send a compound RTCP Sender Report + SDES(CNAME) for this stream on `rtcp_udp`
 * (the relay's RTCP endpoint). Call ~once per second while streaming. */
/* Cloud comedia keepalive: a valid RTP packet (1-byte payload) that latches the SFU's
 * PlainTransport onto our source tuple — sent as the initial latch and periodically. */
int bsdr_video_send_keepalive(bsdr_video_sender *v);

/* Bigscreen CLOUD packetization — for the Mediasoup relay path. Each fragment is
 * [<=1372 NAL bytes][16-byte plaintext trailer] wrapped in plain RTP (the LAN BigSoup format
 * minus the XOR cipher). The Quest reassembles by the trailer's frame/frag/total fields, so
 * src_w/src_h (the encoded resolution; macroblock-aligned internally) MUST be supplied. */
int bsdr_video_send_au_cloud(bsdr_video_sender *v, const uint8_t *annexb, size_t len,
                             int src_w, int src_h);

void bsdr_video_sender_free(bsdr_video_sender *v);

/* --- RTP H.264 depacketizer (used by the loopback test / diagnostics) ------*/
/* Reassemble NAL units from a sequence of RTP payloads. Returns bytes written to
 * `out` (Annex-B), or -1 on overflow. */
typedef struct bsdr_h264_depay bsdr_h264_depay;
bsdr_h264_depay *bsdr_h264_depay_new(void);
/* Feed one RTP payload (after the 12-byte header). Appends completed NALs (in
 * Annex-B, 00 00 00 01 prefixed) to `out`. Returns total bytes in `out`. */
int bsdr_h264_depay_feed(bsdr_h264_depay *d, const uint8_t *payload, size_t len,
                         uint8_t *out, size_t outcap, size_t *outlen);
void bsdr_h264_depay_free(bsdr_h264_depay *d);

#endif /* BSDR_VIDEO_H */
