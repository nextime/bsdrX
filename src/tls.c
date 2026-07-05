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
#ifdef __ANDROID__
#  include <openssl/pem.h>
#  include <dirent.h>
#  include <stdio.h>
#endif

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

#ifdef __ANDROID__
/* Load every PEM cert in `dir` straight into the CTX's trust store. Android's CA dirs are a c_rehash
 * directory whose filenames use OpenSSL's OLD subject hash; the bundled OpenSSL 3.x computes the NEW
 * hash, so a CApath lookup never finds them and every TLS verify fails. Iterating the dir and adding
 * each cert sidesteps the hash entirely. Returns the number of certs added. */
static int android_load_ca_dir(SSL_CTX *ctx, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    struct dirent *e; int added = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        X509 *c;
        while ((c = PEM_read_X509(f, NULL, NULL, NULL)) != NULL) {
            if (X509_STORE_add_cert(store, c) == 1) added++;
            X509_free(c);
        }
        fclose(f);
    }
    closedir(d);
    return added;
}
#endif

void bsdr_tls_configure_client(SSL_CTX *ctx, const char *host) {
    if (!ctx) return;
    if (g_insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        return;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
#ifdef __ANDROID__
    /* The app bundles its own OpenSSL, whose compile-time default CA path doesn't exist on Android,
     * so cert verification (and thus every cloud login) fails. Load Android's system trust store
     * directly (see android_load_ca_dir — a CApath lookup can't be used because of the old/new hash
     * mismatch). Newer Android (14+) keeps it under the conscrypt apex; older in /system. */
    int cacnt = android_load_ca_dir(ctx, "/apex/com.android.conscrypt/cacerts");
    if (cacnt == 0) cacnt = android_load_ca_dir(ctx, "/system/etc/security/cacerts");
    if (cacnt == 0) BSDR_WARN("bsdr.tls", "android: loaded 0 system CAs — TLS will fail");
    else BSDR_DEBUG("bsdr.tls", "android: loaded %d system CAs", cacnt);
#else
    if (SSL_CTX_set_default_verify_paths(ctx) != 1)
        BSDR_WARN("bsdr.tls", "could not load the system CA trust store; "
                              "verification may fail (use --insecure-tls to override)");
#endif
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
