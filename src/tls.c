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
#include "bsdr/tls.h"
#include "bsdr/log.h"

#include <openssl/x509v3.h>

/* Default: verify. The cloud login sends the operator's Bigscreen email+password and
 * receives long-lived tokens, so an unverified chain is a real MITM/credential risk. */
static bool g_insecure = false;

void bsdr_tls_set_insecure(bool insecure) {
    g_insecure = insecure;
    if (insecure)
        BSDR_WARN("bsdr.tls", "TLS certificate verification DISABLED (--insecure-tls) — "
                              "outbound cloud/API traffic is exposed to man-in-the-middle");
}

bool bsdr_tls_is_insecure(void) { return g_insecure; }

void bsdr_tls_configure_client(SSL_CTX *ctx, const char *host) {
    if (!ctx) return;
    if (g_insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        return;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1)
        BSDR_WARN("bsdr.tls", "could not load the system CA trust store; "
                              "verification may fail (use --insecure-tls to override)");
    if (host && *host) {
        /* Pin the certificate to the connection host (SAN/CN + hostname match). Doing this
         * on the CTX is fine because every CTX here serves exactly one host. */
        X509_VERIFY_PARAM *p = SSL_CTX_get0_param(ctx);
        if (p) {
            X509_VERIFY_PARAM_set_hostflags(p, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (X509_VERIFY_PARAM_set1_host(p, host, 0) != 1)
                BSDR_WARN("bsdr.tls", "could not pin certificate host '%s'", host);
        }
    }
}
