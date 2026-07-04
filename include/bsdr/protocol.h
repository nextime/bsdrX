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
/* Bigscreen Remote Desktop LAN protocol constants.
 *
 * Reverse-engineered from the official PC agent (see ../../projectB-protocol-spec.md).
 * Observed-behaviour constants, not copied source.
 */
#ifndef BSDR_PROTOCOL_H
#define BSDR_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* --- LAN discovery (UDP) --------------------------------------------------*/
/* The headset broadcasts exactly these 5 bytes to DISCOVERY_REQUEST_PORT; the
 * PC validates the header and replies to REMOTE_CLIENT_INFO_PORT with the same
 * header followed by a UTF-8 JSON identity blob. */
static const uint8_t BSDR_BROADCAST_HEADER[5] = { 0xB1, 0x65, 0xC7, 0x33, 0x90 };
#define BSDR_BROADCAST_HEADER_LEN 5

#define BSDR_DISCOVERY_REQUEST_PORT   45000  /* we listen here */
#define BSDR_REMOTE_CLIENT_INFO_PORT  45001  /* we reply here */
#define BSDR_BROADCAST_INTERVAL_MS    5000

/* --- Pairing + control (HTTP) ---------------------------------------------*/
#define BSDR_HTTP_SERVER_PORT  45678

/* --- Media + input (DTLS-SRTP + SCTP over UDP, no ICE) --------------------*/
#define BSDR_REMOTE_DESKTOP_PORT  45002   /* video + magic-beacon channel */
/* Desktop audio is a SEPARATE socket on video_port+1 (45003): BigSoup.dll's
 * CreateRemoteDesktopConnection(ip,45002) internally also opens the audio
 * connection on param_3+1 (FUN_18008fdf0 -> FUN_18008e360, "Creating remote
 * desktop audio connection ... %s:%i"). Sending Opus on 45002 makes the Quest
 * parse it as video and crash. Mic-in comes back on this same 45003 socket. */
#define BSDR_REMOTE_AUDIO_PORT    45003
/* The DTLS-SRTP + SCTP data channel is on 45004 (confirmed by pcap: the Quest
 * sends its ClientHello from 45004 -> PC:45004; PC is the DTLS server here). */
#define BSDR_REMOTE_DATA_PORT     45004
#define BSDR_DEFAULT_SCTP_PORT    5000    /* WebRTC convention */
/* Pre-DTLS UDP hello beacon on 45002: each peer sends this 4-byte magic ~1/s and
 * waits to receive the peer's before proceeding to DTLS (BigSoup.dll: sent at
 * 0x18009a717, checked at 0x18008fce5; wire bytes LE = 67 45 23 01). */
#define BSDR_LAN_HELLO_MAGIC      0x01234567u

/* --- Timeouts (from server.js) --------------------------------------------*/
#define BSDR_CHECK_HEARTBEAT_INTERVAL_MS  5000
#define BSDR_FORGET_UNRESPONSIVE_DEVICE_MS 15000

/* --- Native tunable bounds (from build/native/index.js) -------------------*/
#define BSDR_MIN_BITRATE     500000
#define BSDR_MAX_BITRATE     100000000
#define BSDR_MIN_FEC         1
#define BSDR_MAX_FEC         5
#define BSDR_MIN_FPS         1
#define BSDR_MAX_FPS         300
#define BSDR_MIN_RESOLUTION  16
#define BSDR_MAX_RESOLUTION  4320

/* --- DataChannel input message types (byte[0]) ----------------------------*/
/* Recovered by static disassembly of BigSoup.dll; see the spec's table. */
enum {
    BSDR_MSG_MOVE_ABS    = 0x00,
    BSDR_MSG_MOVE_REL    = 0x01,
    BSDR_MSG_LEFT_DOWN   = 0x02,
    BSDR_MSG_LEFT_UP     = 0x03,
    BSDR_MSG_RIGHT_DOWN  = 0x04,
    BSDR_MSG_RIGHT_UP    = 0x05,
    BSDR_MSG_MIDDLE_DOWN = 0x06,
    BSDR_MSG_MIDDLE_UP   = 0x07,
    BSDR_MSG_X1_DOWN     = 0x08,
    BSDR_MSG_X1_UP       = 0x09,
    BSDR_MSG_X2_DOWN     = 0x0A,
    BSDR_MSG_X2_UP       = 0x0B,
    BSDR_MSG_WHEEL_V     = 0x0C,
    BSDR_MSG_WHEEL_H     = 0x0D,
    BSDR_MSG_KEY         = 0x0E,
    BSDR_MSG_GAMEPAD     = 0x20
};

/* --- Media parameters (reversed from BigSoup.dll; see the spec) ------------*/
/* Video: OpenH264 Constrained Baseline H.264, RTP RFC 6184, 90 kHz clock. */
#define BSDR_VIDEO_CLOCK_HZ     90000
#define BSDR_VIDEO_PT_DEFAULT   111     /* H.264; from BigSoup.dll RTPSession::SetDefaultPayloadType (was 96) */
/* Audio: Opus AUDIO mode, 48 kHz, 10 ms frames. Desktop = stereo 64 kbps; mic = mono 32 kbps. */
#define BSDR_AUDIO_CLOCK_HZ     48000
#define BSDR_AUDIO_CHANNELS     2
#define BSDR_AUDIO_DESKTOP_BPS  64000
#define BSDR_AUDIO_MIC_BPS      32000
#define BSDR_AUDIO_PT_DEFAULT   100     /* Opus (desktop+mic share PT); from BigSoup.dll disasm (was 97) */
#define BSDR_VIDEO_SSRC         0x42425344u  /* "BBSD" */
#define BSDR_AUDIO_SSRC         0x42425341u  /* "BBSA" */

static inline bool bsdr_check_message_header(const uint8_t *data, size_t len) {
    if (len != BSDR_BROADCAST_HEADER_LEN) return false;
    for (size_t i = 0; i < BSDR_BROADCAST_HEADER_LEN; i++)
        if (data[i] != BSDR_BROADCAST_HEADER[i]) return false;
    return true;
}

#endif /* BSDR_PROTOCOL_H */
