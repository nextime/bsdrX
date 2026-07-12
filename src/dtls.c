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
#include "bsdr/dtls.h"
#include "bsdr/log.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <string.h>

/* BigSoup.dll only implements AES-CM-128-HMAC-SHA1-80 SRTP (no GCM policy exists
 * in it — disasm). Offering GCM can fault the headset's older use_srtp parser, so
 * we offer only what it supports, CM-128 (with the 32-bit-tag variant). */
#define BSDR_SRTP_PROFILES \
    "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32"

struct bsdr_dtls {
    bsdr_udp *udp;
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;   /* OpenSSL reads incoming datagrams from here */
    BIO *wbio;   /* OpenSSL writes outgoing datagrams here */
    bool connected;
    bsdr_dtls_role role;
    uint8_t hello[16];          /* optional pre-DTLS UDP hello beacon */
    size_t  hello_len;
    int     hello_interval_ms;
    uint64_t hello_last;
};

void bsdr_dtls_set_hello(bsdr_dtls *d, const void *buf, size_t len, int interval_ms) {
    if (!d || len > sizeof(d->hello)) return;
    memcpy(d->hello, buf, len);
    d->hello_len = len;
    d->hello_interval_ms = interval_ms > 0 ? interval_ms : 1000;
    d->hello_last = 0;
}

/* ---- self-signed cert (EC P-256), generated at runtime --------------------*/
static bool make_self_signed(EVP_PKEY **pkey_out, X509 **x509_out) {
    EVP_PKEY *pkey = EVP_EC_gen("P-256");
    if (!pkey) return false;
    X509 *x = X509_new();
    if (!x) { EVP_PKEY_free(pkey); return false; }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 60L * 60 * 24 * 365);
    X509_set_pubkey(x, pkey);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)"bsdrX", -1, -1, 0);
    X509_set_issuer_name(x, name);
    if (!X509_sign(x, pkey, EVP_sha256())) {
        X509_free(x); EVP_PKEY_free(pkey); return false;
    }
    *pkey_out = pkey;
    *x509_out = x;
    return true;
}

/* One process-wide self-signed cert/key, generated once and shared by every DTLS SSL_CTX
 * (SSL_CTX_use_* up-ref it). The peer verifies no fingerprint, so sharing is protocol-identical and
 * saves an EC-P256 keygen + X509 sign per bsdr_dtls_new (LAN input + cloud input each make one, on
 * separate threads — hence the thread-safe once-init via OpenSSL's own primitive). */
static EVP_PKEY *g_dtls_key = NULL;
static X509     *g_dtls_cert = NULL;
static CRYPTO_ONCE g_dtls_once = CRYPTO_ONCE_STATIC_INIT;
static void dtls_cert_init(void) {
    if (!make_self_signed(&g_dtls_key, &g_dtls_cert)) { g_dtls_key = NULL; g_dtls_cert = NULL; }
}

bsdr_dtls *bsdr_dtls_new(bsdr_udp *udp, bsdr_dtls_role role) {
    bsdr_dtls *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->udp = udp;
    d->role = role;

    d->ctx = SSL_CTX_new(DTLS_method());
    if (!d->ctx) { free(d); return NULL; }
    /* Bigscreen verifies no peer fingerprint (none is exchanged) — neither do we. */
    SSL_CTX_set_verify(d->ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_tlsext_use_srtp(d->ctx, BSDR_SRTP_PROFILES);

    CRYPTO_THREAD_run_once(&g_dtls_once, dtls_cert_init);   /* generate the shared cert/key once */
    if (!g_dtls_cert || !g_dtls_key ||
        SSL_CTX_use_certificate(d->ctx, g_dtls_cert) != 1 ||   /* up-refs the shared cert */
        SSL_CTX_use_PrivateKey(d->ctx, g_dtls_key) != 1) {     /* up-refs the shared key */
        BSDR_ERROR("bsdr.dtls", "cert setup failed");
        SSL_CTX_free(d->ctx);
        free(d);
        return NULL;
    }

    d->ssl = SSL_new(d->ctx);
    d->rbio = BIO_new(BIO_s_mem());
    d->wbio = BIO_new(BIO_s_mem());
    if (!d->ssl || !d->rbio || !d->wbio) {   /* OOM: don't deref NULL in BIO_set_/SSL_set_bio */
        BSDR_ERROR("bsdr.dtls", "SSL/BIO alloc failed");
        if (d->ssl) SSL_free(d->ssl);        /* frees attached BIOs; else free them directly */
        else { if (d->rbio) BIO_free(d->rbio); if (d->wbio) BIO_free(d->wbio); }
        SSL_CTX_free(d->ctx);
        free(d);
        return NULL;
    }
    /* memory read-BIO: return "want read" instead of EOF when empty */
    BIO_set_mem_eof_return(d->rbio, -1);
    BIO_set_mem_eof_return(d->wbio, -1);
    SSL_set_bio(d->ssl, d->rbio, d->wbio);
    if (role == BSDR_DTLS_SERVER) SSL_set_accept_state(d->ssl);
    else SSL_set_connect_state(d->ssl);
    return d;
}

static void flush_out(bsdr_dtls *d) {
    char buf[2048];
    int n;
    while ((n = BIO_read(d->wbio, buf, sizeof(buf))) > 0)
        bsdr_udp_send(d->udp, buf, (size_t)n);
}

bool bsdr_dtls_handshake(bsdr_dtls *d, int timeout_ms, volatile int *cancel) {
    uint64_t deadline = bsdr_now_ms() + (uint64_t)timeout_ms;
    while (bsdr_now_ms() < deadline) {
        if (cancel && *cancel) { BSDR_INFO("bsdr.dtls", "handshake cancelled"); return false; }
        if (d->hello_len) {     /* beacon the LAN hello so the headset starts DTLS */
            uint64_t now = bsdr_now_ms();
            if (now - d->hello_last >= (uint64_t)d->hello_interval_ms) {
                bsdr_udp_send(d->udp, d->hello, d->hello_len);
                d->hello_last = now;
            }
        }
        int ret = SSL_do_handshake(d->ssl);
        flush_out(d);
        if (ret == 1) {
            d->connected = true;
            BSDR_INFO("bsdr.dtls", "handshake complete");
            return true;
        }
        int err = SSL_get_error(d->ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            unsigned long e = ERR_get_error();
            BSDR_ERROR("bsdr.dtls", "handshake error: %s",
                       e ? ERR_reason_error_string(e) : "fatal");
            return false;
        }
        /* respect the DTLS retransmit timer */
        int wait = (int)(deadline - bsdr_now_ms());
        if (wait <= 0) break;
        struct timeval tv;
        if (DTLSv1_get_timeout(d->ssl, &tv)) {
            int dto = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
            if (dto >= 0 && dto < wait) wait = dto;
        }
        if (wait < 5) wait = 5;
        if (wait > 200) wait = 200;   /* re-check cancel/handshake promptly */
        char dg[2048];
        int n = bsdr_udp_recv(d->udp, dg, sizeof(dg), wait);
        if (n > 0) BIO_write(d->rbio, dg, n);
        else if (n == 0) DTLSv1_handle_timeout(d->ssl);   /* retransmit */
        else return false;
    }
    BSDR_ERROR("bsdr.dtls", "handshake timed out after %dms", timeout_ms);
    return false;
}

int bsdr_dtls_send(bsdr_dtls *d, const void *buf, size_t len) {
    int n = SSL_write(d->ssl, buf, (int)len);
    flush_out(d);
    return n;
}

int bsdr_dtls_recv(bsdr_dtls *d, void *buf, size_t len, int timeout_ms) {
    uint64_t deadline = bsdr_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int n = SSL_read(d->ssl, buf, (int)len);
        if (n > 0) return n;
        int err = SSL_get_error(d->ssl, n);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            if (err == SSL_ERROR_ZERO_RETURN) return 0;   /* closed */
            return -1;
        }
        int wait = (int)(deadline - bsdr_now_ms());
        if (wait <= 0) return 0;
        char dg[2048];
        int r = bsdr_udp_recv(d->udp, dg, sizeof(dg), wait);
        if (r > 0) BIO_write(d->rbio, dg, r);
        else if (r < 0) return -1;
        flush_out(d);   /* SSL_read may have queued acks/alerts */
    }
}

void bsdr_dtls_process(bsdr_dtls *d, const uint8_t *dgram, int dlen,
                       bsdr_dtls_appdata_cb cb, void *user) {
    BIO_write(d->rbio, dgram, dlen);
    uint8_t out[2048];
    int n;
    while ((n = SSL_read(d->ssl, out, sizeof(out))) > 0)
        if (cb) cb(out, n, user);
    flush_out(d);   /* SSL_read may have queued acks/alerts */
}

void bsdr_dtls_peer_info(bsdr_dtls *d, char *subject, size_t slen,
                         char *srtp, size_t plen) {
    if (subject && slen) {
        subject[0] = '\0';
        X509 *peer = SSL_get1_peer_certificate(d->ssl);
        if (peer) {
            X509_NAME_oneline(X509_get_subject_name(peer), subject, (int)slen);
            X509_free(peer);
        }
    }
    if (srtp && plen) {
        srtp[0] = '\0';
        const SRTP_PROTECTION_PROFILE *p = SSL_get_selected_srtp_profile(d->ssl);
        if (p) snprintf(srtp, plen, "%s", p->name);
    }
}

bool bsdr_dtls_export_srtp(bsdr_dtls *d, bsdr_srtp_keys *out) {
    memset(out, 0, sizeof(*out));
    const SRTP_PROTECTION_PROFILE *p = SSL_get_selected_srtp_profile(d->ssl);
    size_t keylen, saltlen;
    if (p && p->id == SRTP_AEAD_AES_128_GCM) {
        out->profile = BSDR_SRTP_AEAD_AES_128_GCM;
        keylen = 16; saltlen = 12;
    } else {
        /* BigSoup never sends the use_srtp extension (pcap), yet still keys SRTP
         * from the DTLS exporter as AES_CM_128_SHA1_80 — so default to that when
         * no profile was negotiated rather than failing. */
        out->profile = BSDR_SRTP_AES128_CM_SHA1_80;
        keylen = 16; saltlen = 14;
    }

    /* RFC 5764: export client_key|server_key|client_salt|server_salt. */
    uint8_t material[2 * (16 + 14)];
    size_t total = 2 * (keylen + saltlen);
    if (SSL_export_keying_material(d->ssl, material, total,
                                   "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0) != 1) {
        BSDR_ERROR("bsdr.dtls", "SSL_export_keying_material failed");
        return false;
    }
    const uint8_t *ckey = material;
    const uint8_t *skey = material + keylen;
    const uint8_t *csalt = material + 2 * keylen;
    const uint8_t *ssalt = material + 2 * keylen + saltlen;

    /* Client write key/salt are used by the DTLS client to send; server's by the
     * DTLS server. We are the RTP sender either way -> pick by our role. */
    const uint8_t *send_key, *send_salt, *recv_key, *recv_salt;
    if (d->role == BSDR_DTLS_CLIENT) {
        send_key = ckey; send_salt = csalt; recv_key = skey; recv_salt = ssalt;
    } else {
        send_key = skey; send_salt = ssalt; recv_key = ckey; recv_salt = csalt;
    }
    memcpy(out->send_master, send_key, keylen);
    memcpy(out->send_master + keylen, send_salt, saltlen);
    out->send_master_len = keylen + saltlen;
    memcpy(out->recv_master, recv_key, keylen);
    memcpy(out->recv_master + keylen, recv_salt, saltlen);
    out->recv_master_len = keylen + saltlen;
    /* p is NULL when use_srtp wasn't negotiated (the headset doesn't send it) —
     * don't deref it; report the profile we defaulted to instead. */
    BSDR_INFO("bsdr.dtls", "exported SRTP keys (%s)",
              p ? p->name : "AES128_CM_SHA1_80 (no use_srtp)");
    return true;
}

void bsdr_dtls_free(bsdr_dtls *d) {
    if (!d) return;
    if (d->ssl) SSL_free(d->ssl);     /* frees the BIOs too */
    if (d->ctx) SSL_CTX_free(d->ctx);
    free(d);
}
