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
/* Outbound-TLS verification policy for the HTTPS/WSS clients (cloud login, token
 * renew, presence WebSocket, and the STT/LLM HTTP client). Verification is ON by
 * default; --insecure-tls disables it for self-signed dev proxies / offline tests.
 * This is process-wide because the SSL_CTX objects are created ad hoc at each call
 * site rather than threaded through a config struct. Does NOT govern the DTLS-SRTP
 * media/data path (dtls.c), which is authenticated by the reversed key exchange. */
#ifndef BSDR_TLS_H
#define BSDR_TLS_H

#include <stdbool.h>
#include <openssl/ssl.h>

/* Set the process-wide policy (call once from argv parsing). */
void bsdr_tls_set_insecure(bool insecure);
bool bsdr_tls_is_insecure(void);

/* Apply the current policy to a freshly created client SSL_CTX. When verifying,
 * loads the system trust store and pins the peer certificate to `host` (SAN/CN +
 * hostname check); when insecure, disables verification. host may be NULL. */
void bsdr_tls_configure_client(SSL_CTX *ctx, const char *host);

#endif /* BSDR_TLS_H */
