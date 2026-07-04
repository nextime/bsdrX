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
/* Shared SRTP helpers (libsrtp) for the video and audio senders/receivers. */
#ifndef BSDR_SRTP_UTIL_H
#define BSDR_SRTP_UTIL_H

#include "bsdr/dtls.h"   /* bsdr_srtp_keys / bsdr_srtp_profile */
#include <srtp2/srtp.h>
#include <stdbool.h>

/* Refcounted libsrtp init/shutdown (call around any srtp session use). */
bool bsdr_srtp_global_init(void);
void bsdr_srtp_global_shutdown(void);

/* Create an SRTP session from a master key (key||salt) of the given profile.
 * inbound=false: outbound, bound to `ssrc`. inbound=true: accept any SSRC (we
 * don't know the peer's). Use keys->send_master to send, recv_master to receive. */
bool bsdr_srtp_session_create(srtp_t *out, const uint8_t *master,
                              bsdr_srtp_profile profile, uint32_t ssrc,
                              bool inbound);

#endif /* BSDR_SRTP_UTIL_H */
