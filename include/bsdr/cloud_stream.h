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
/* Cloud (internet) relay streaming — Mediasoup-style, reversed from BigSoup.dll.
 *
 * Confirmed by static RE (no ICE/STUN anywhere in BigSoup.dll; jrtplib RTPUDPv4
 * + DTLS-SRTP + usrsctp): the host dials the relay directly (comedia plain RTP,
 * no ICE) on three separate UDP ports from GET /rooms — videoPort, audioPort,
 * dataPort — each carrying a DTLS-SRTP channel. Video=RTP pt 111, audio=pt 100.
 * The data/input channel is DTLS (host=server, same as LAN 45004) carrying the
 * input opcodes. This module points the existing per-channel senders at the
 * relay ports.
 *
 * UNVALIDATED (needs a live internet-sharing capture): the media DTLS role
 * (server vs client) and whether the data channel wraps input in usrsctp vs raw.
 * Defaults: media DTLS role = server (matching the confirmed data channel + LAN);
 * override with BSDR_CLOUD_DTLS_ROLE=client. Data channel = raw DTLS app-data. */
#ifndef BSDR_CLOUD_STREAM_H
#define BSDR_CLOUD_STREAM_H

#include <stdint.h>
#include <stddef.h>
#include "bsdr/cloud.h"   /* bsdr_cloud_screen */

typedef struct bsdr_app bsdr_app;
typedef struct bsdr_cloud_stream bsdr_cloud_stream;

/* Begin relay streaming to the resolved screen. `app` is borrowed (for the live
 * capture region/quality). Returns NULL if capture/audio support is absent or
 * the screen has no media ports. Stop with bsdr_cloud_stream_stop. */
bsdr_cloud_stream *bsdr_cloud_stream_start(const bsdr_cloud_screen *scr,
                                           bsdr_app *app, bool audio);
void bsdr_cloud_stream_stop(bsdr_cloud_stream *cs);

/* Pause/resume without tearing down (keeps sockets + SCTP alive across share toggles). */
void bsdr_cloud_stream_set_active(bsdr_cloud_stream *cs, int active);
int  bsdr_cloud_stream_active(bsdr_cloud_stream *cs);
/* True if this running stream targets the same relay tuple as `scr` (resume vs restart). */
bool bsdr_cloud_stream_matches(bsdr_cloud_stream *cs, const bsdr_cloud_screen *scr);

/* Feed one already-encoded Annex-B access unit (from the single LAN encoder) to the relay (COUPLED
 * mode, the default). w/h are the encoded resolution (for the cloud trailer). No-op in --video-
 * decoupled mode (the relay runs its own capture+encoder then). Call under the app lock. */
void bsdr_cloud_stream_feed_video(bsdr_cloud_stream *cs, const uint8_t *au, size_t len, int w, int h);

#endif /* BSDR_CLOUD_STREAM_H */
