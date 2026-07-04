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
/* Minimal HTTP/HTTPS client. */
#include "bsdr/httpc.h"
#include "bsdr/net.h"
#include "bsdr/log.h"
#include "bsdr/tls.h"

#include <openssl/ssl.h>
#include <openssl/bio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <netdb.h>   /* getaddrinfo */
#endif

static bool parse_url(const char *url, int *https, char *host, size_t hlen,
                      int *port, char *path, size_t plen) {
    *https = 0; *port = 80;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { *https = 1; *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return false;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *hostend = slash ? slash : p + strlen(p);
    if (colon && colon < hostend) {
        *port = atoi(colon + 1);
        hostend = colon;
    }
    size_t hn = (size_t)(hostend - p);
    if (hn >= hlen) hn = hlen - 1;
    memcpy(host, p, hn); host[hn] = '\0';
    snprintf(path, plen, "%s", slash ? slash : "/");
    return true;
}

/* abstract connection (plain socket or TLS BIO) */
typedef struct {
    int https;
    bsdr_socket_t sock;   /* plain */
    SSL_CTX *ctx; BIO *bio;  /* tls */
} conn_t;

static bool conn_open(conn_t *c, int https, const char *host, int port) {
    memset(c, 0, sizeof(*c));
    c->https = https;
    if (https) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        if (!c->ctx) return false;
        bsdr_tls_configure_client(c->ctx, host);
        c->bio = BIO_new_ssl_connect(c->ctx);
        SSL *ssl = NULL; BIO_get_ssl(c->bio, &ssl);
        if (!ssl) return false;
        SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
        SSL_set_tlsext_host_name(ssl, host);
        char hp[300]; snprintf(hp, sizeof(hp), "%s:%d", host, port);
        BIO_set_conn_hostname(c->bio, hp);
        return BIO_do_connect(c->bio) > 0;
    }
    struct sockaddr_in addr;
    if (!bsdr_sockaddr_make(&addr, host, (uint16_t)port)) {
        /* resolve via getaddrinfo for hostnames */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char ports[8]; snprintf(ports, sizeof(ports), "%d", port);
        if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) return false;
        memcpy(&addr, res->ai_addr, sizeof(addr));
        freeaddrinfo(res);
    }
    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->sock == BSDR_INVALID_SOCKET) return false;
    return connect(c->sock, (struct sockaddr *)&addr, sizeof(addr)) == 0;
}
static int conn_write(conn_t *c, const void *b, int n) {
    return c->https ? BIO_write(c->bio, b, n) : (int)send(c->sock, b, n, 0);
}
static int conn_read(conn_t *c, void *b, int n) {
    if (c->https) {
        for (;;) {   /* loop, not recurse: a stalling TLS peer must not grow the stack */
            int r = BIO_read(c->bio, b, n);
            if (r > 0 || !BIO_should_retry(c->bio)) return r;
        }
    }
    return (int)recv(c->sock, b, n, 0);
}
static void conn_close(conn_t *c) {
    if (c->https) { if (c->bio) BIO_free_all(c->bio); if (c->ctx) SSL_CTX_free(c->ctx); }
    else if (c->sock) bsdr_socket_close(c->sock);
}

int bsdr_http_request(const char *method, const char *url,
                      const bsdr_http_header *headers, int nheaders,
                      const char *content_type,
                      const void *body, size_t body_len,
                      char *resp, size_t resp_cap) {
    int https, port; char host[256], path[1024];
    if (!parse_url(url, &https, host, sizeof(host), &port, path, sizeof(path))) return -1;

    conn_t c;
    if (!conn_open(&c, https, host, port)) {
        BSDR_ERROR("bsdr.http", "connect %s failed", url);
        conn_close(&c); return -1;
    }
    char hdr[2048];
    int n = snprintf(hdr, sizeof(hdr),
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, host);
    for (int i = 0; i < nheaders; i++)
        n += snprintf(hdr + n, sizeof(hdr) - n, "%s: %s\r\n",
                      headers[i].name, headers[i].value);
    if (content_type)
        n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Type: %s\r\n", content_type);
    n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Length: %zu\r\n\r\n", body_len);

    if (conn_write(&c, hdr, n) <= 0) { conn_close(&c); return -1; }
    if (body_len && conn_write(&c, body, (int)body_len) <= 0) { conn_close(&c); return -1; }

    size_t total = 0;
    for (;;) {
        int r = conn_read(&c, resp + total, (int)(resp_cap - 1 - total));
        if (r > 0) { total += (size_t)r; if (total >= resp_cap - 1) break; }
        else break;
    }
    resp[total] = '\0';
    conn_close(&c);
    return (int)total;
}

int bsdr_http_status(const char *resp) {
    const char *sp = strchr(resp, ' ');
    return sp ? atoi(sp + 1) : 0;
}
const char *bsdr_http_body(const char *resp) {
    const char *b = strstr(resp, "\r\n\r\n");
    return b ? b + 4 : NULL;
}
