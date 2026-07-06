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
/* Bigscreen cloud HTTPS client (OpenSSL). */
#include "bsdr/cloud.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>    /* getenv */
#include <string.h>
#include <strings.h>   /* strncasecmp */

/* Bigscreen client API key: the BSDR_CLOUD_API_KEY env var if set, else the compiled default
 * (blank in the public mirror). See cloud.h / the README. */
const char *bsdr_cloud_api_key(void) {
    const char *e = getenv("BSDR_CLOUD_API_KEY");
    return (e && *e) ? e : BSDR_CLOUD_API_KEY_DEFAULT;
}

/* Do one HTTPS request to host:443; return the full response (headers+body) in
 * `resp` (size `cap`). Returns response length, or -1. */
static int https_request(const char *host, const char *request,
                         char *resp, size_t cap) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    bsdr_tls_configure_client(ctx, host);

    BIO *bio = BIO_new_ssl_connect(ctx);
    SSL *ssl = NULL;
    BIO_get_ssl(bio, &ssl);
    if (!ssl) { BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
    SSL_set_tlsext_host_name(ssl, host);
    char hostport[300];
    snprintf(hostport, sizeof(hostport), "%s:443", host);
    BIO_set_conn_hostname(bio, hostport);

    int rc = -1;
    if (BIO_do_connect(bio) <= 0) {
        BSDR_ERROR("bsdr.cloud", "connect %s failed", host);
        goto done;
    }
    if (BIO_write(bio, request, (int)strlen(request)) <= 0) goto done;

    size_t total = 0;
    for (;;) {
        int n = BIO_read(bio, resp + total, (int)(cap - 1 - total));
        if (n > 0) { total += (size_t)n; if (total >= cap - 1) break; }
        else if (BIO_should_retry(bio)) continue;
        else break;
    }
    resp[total] = '\0';
    rc = (int)total;
done:
    BIO_free_all(bio);
    SSL_CTX_free(ctx);
    return rc;
}

/* case-insensitive header value lookup: "Name: value\r\n" */
static bool header_value(const char *resp, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *p = resp; *p; p++) {
        if (p == resp || p[-1] == '\n') {
            if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
                const char *v = p + nl + 1;
                while (*v == ' ') v++;
                size_t o = 0;
                while (*v && *v != '\r' && *v != '\n' && o + 1 < cap) out[o++] = *v++;
                out[o] = '\0';
                return true;
            }
        }
    }
    return false;
}

static int status_code(const char *resp) {
    /* "HTTP/1.1 200 OK" */
    const char *sp = strchr(resp, ' ');
    return sp ? atoi(sp + 1) : 0;
}

bool bsdr_cloud_login(const char *email, const char *password,
                      bsdr_cloud_result *out) {
    memset(out, 0, sizeof(*out));

    char body[512];
    char emE[160], pwE[160];
    bsdr_json_escape(emE, sizeof(emE), email);
    bsdr_json_escape(pwE, sizeof(pwE), password);
    int blen = snprintf(body, sizeof(body),
                        "{\"email\":\"%s\",\"password\":\"%s\"}", emE, pwE);

    char req[1024];
    snprintf(req, sizeof(req),
        "POST /auth/login HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        BSDR_CLOUD_API_HOST, bsdr_cloud_api_key(), blen, body);

    static char resp[16384];
    int n = https_request(BSDR_CLOUD_API_HOST, req, resp, sizeof(resp));
    if (n < 0) { snprintf(out->message, sizeof(out->message), "connection failed"); return false; }

    out->http_status = status_code(resp);
    header_value(resp, "x-access-token", out->access_token, sizeof(out->access_token));
    header_value(resp, "x-refresh-token", out->refresh_token, sizeof(out->refresh_token));
    out->ok = (out->http_status >= 200 && out->http_status < 300 && out->access_token[0]);
    snprintf(out->message, sizeof(out->message), out->ok ? "logged in" : "login failed (HTTP %d)",
             out->http_status);
    BSDR_INFO("bsdr.cloud", "login %s -> HTTP %d (%s)", email, out->http_status,
              out->ok ? "ok" : "fail");
    return out->ok;
}

bool bsdr_cloud_account(const char *access_token, char *name, size_t name_len) {
    char req[1024];
    snprintf(req, sizeof(req),
        "GET /auth/account HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "x-access-token: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n",
        BSDR_CLOUD_API_HOST, bsdr_cloud_api_key(), access_token);

    static char resp[16384];
    int n = https_request(BSDR_CLOUD_API_HOST, req, resp, sizeof(resp));
    if (n < 0 || status_code(resp) / 100 != 2) return false;
    const char *body = strstr(resp, "\r\n\r\n");
    if (!body) return false;
    /* best-effort: pull a display name / username field */
    if (bsdr_json_get_str(body, "displayName", name, name_len)) return true;
    if (bsdr_json_get_str(body, "username", name, name_len)) return true;
    if (bsdr_json_get_str(body, "email", name, name_len)) return true;
    return false;
}

/* ---- base64 (for the WS connectionString) ---- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64enc(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i+1] << 8;
        if (i + 2 < n) v |= in[i+2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

/* systemInfo JSON (matches app.js getSystemInfo) + base64 connectionString. */
static void make_connection_string(const char *access_token, char *out, size_t cap) {
    char host[128] = "bsdrX-host";
#if !defined(_WIN32)
    extern int gethostname(char *, size_t);
    gethostname(host, sizeof(host));
#endif
    /* Must match the official Bigscreen Remote Desktop Client so the cloud lists
     * us as an addable RDC host on the Quest: version = config.versionNumber
     * (0.900.0), operatingSystem = "Windows" (RDC is Windows-only; the Quest's
     * add-screen list filters on it). We present as that client (interop). */
    char info[512];
    snprintf(info, sizeof(info),
        "{\"deviceUniqueIdentifier\":\"%s\",\"version\":\"0.900.0\","
        "\"deviceName\":\"Bigscreen Remote Desktop Client\",\"deviceModel\":\"Electron\","
        "\"operatingSystem\":\"Windows\",\"CPU\":\"Unknown\",\"memory\":\"Unknown\",\"GPU\":\"NVidia\"}",
        host);
    char packet[4096];
    int pl = snprintf(packet, sizeof(packet),
        "{\"accessToken\":\"%s\",\"systemInfo\":%s}", access_token, info);
    /* snprintf returns the would-have-written length; clamp so b64enc never reads past packet[]
     * if the token ever overflows the buffer (it can't today, but the read length must be safe). */
    size_t plen = (pl < 0) ? 0 : ((size_t)pl < sizeof(packet) ? (size_t)pl : sizeof(packet) - 1);
    b64enc((const unsigned char *)packet, plen, out);
    (void)cap;
}

/* The systemInfo JSON on its own, base64'd — the x-bigscreen-system-info header for /auth/renew. */
static void make_system_info_b64(char *out, size_t cap) {
    char host[128] = "bsdrX-host";
#if !defined(_WIN32)
    extern int gethostname(char *, size_t);
    gethostname(host, sizeof(host));
#endif
    char info[512];
    snprintf(info, sizeof(info),
        "{\"deviceUniqueIdentifier\":\"%s\",\"version\":\"0.900.0\","
        "\"deviceName\":\"Bigscreen Remote Desktop Client\",\"deviceModel\":\"Electron\","
        "\"operatingSystem\":\"Windows\",\"CPU\":\"Unknown\",\"memory\":\"Unknown\",\"GPU\":\"NVidia\"}",
        host);
    b64enc((const unsigned char *)info, strlen(info), out);
    (void)cap;
}

bool bsdr_cloud_renew(const char *refresh_token, bsdr_cloud_result *out) {
    memset(out, 0, sizeof(*out));
    if (!refresh_token || !refresh_token[0]) {
        snprintf(out->message, sizeof(out->message), "no refresh token"); return false;
    }
    char sysinfo_b64[1024];
    make_system_info_b64(sysinfo_b64, sizeof(sysinfo_b64));

    /* Renewal is a two-step nonce flow (from the Electron client's checkAccessTokenStatus +
     * renewAccessToken):
     *   1) GET /auth/verify with x-access-token: renew  -> 401 + header x-bigscreen-nonce
     *   2) GET /auth/renew with that nonce + x-refresh-token + x-bigscreen-system-info
     *      -> 2xx, new tokens returned in x-access-token / x-refresh-token response headers.
     * A plain POST /auth/renew (the old code) is not a route -> 404. */
    static char vresp[16384];
    char vreq[1024];
    snprintf(vreq, sizeof(vreq),
        "GET /auth/verify HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "x-access-token: renew\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n",
        BSDR_CLOUD_API_HOST, bsdr_cloud_api_key());
    if (https_request(BSDR_CLOUD_API_HOST, vreq, vresp, sizeof(vresp)) < 0) {
        snprintf(out->message, sizeof(out->message), "verify connection failed"); return false;
    }
    char nonce[512] = {0};
    if (!header_value(vresp, "x-bigscreen-nonce", nonce, sizeof(nonce)) || !nonce[0]) {
        out->http_status = status_code(vresp);
        snprintf(out->message, sizeof(out->message), "no renewal nonce (verify HTTP %d)", out->http_status);
        BSDR_WARN("bsdr.cloud", "renew: /auth/verify gave no nonce (HTTP %d)", out->http_status);
        return false;
    }

    char req[2048];
    snprintf(req, sizeof(req),
        "GET /auth/renew HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "x-bigscreen-nonce: %s\r\n"
        "x-refresh-token: %s\r\n"
        "x-bigscreen-system-info: %s\r\n"     /* required on renew (NOT on login) */
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n",
        BSDR_CLOUD_API_HOST, bsdr_cloud_api_key(), nonce, refresh_token, sysinfo_b64);

    static char resp[16384];
    int n = https_request(BSDR_CLOUD_API_HOST, req, resp, sizeof(resp));
    if (n < 0) { snprintf(out->message, sizeof(out->message), "connection failed"); return false; }
    out->http_status = status_code(resp);
    header_value(resp, "x-access-token", out->access_token, sizeof(out->access_token));
    header_value(resp, "x-refresh-token", out->refresh_token, sizeof(out->refresh_token));
    out->ok = (out->http_status >= 200 && out->http_status < 300 && out->access_token[0]);
    snprintf(out->message, sizeof(out->message), out->ok ? "renewed" : "renew failed (HTTP %d)",
             out->http_status);
    BSDR_INFO("bsdr.cloud", "renew -> HTTP %d (%s)", out->http_status, out->ok ? "ok" : "fail");
    return out->ok;
}

bool bsdr_cloud_get_rooms(const char *access_token, bsdr_cloud_screen *out) {
    memset(out, 0, sizeof(*out));
    char req[4096];
    snprintf(req, sizeof(req),
        "GET /rooms HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "x-access-token: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n",
        BSDR_CLOUD_API2_HOST, bsdr_cloud_api_key(), access_token);
    static char resp[65536];
    int n = https_request(BSDR_CLOUD_API2_HOST, req, resp, sizeof(resp));
    if (n < 0) { BSDR_WARN("bsdr.cloud", "GET /rooms: connection failed"); return false; }
    int code = status_code(resp);
    out->http_status = code;
    const char *body = strstr(resp, "\r\n\r\n");
    if (code / 100 != 2 || !body) { BSDR_WARN("bsdr.cloud", "GET /rooms -> HTTP %d", code); return false; }
    body += 4;
    /* Raw body so we can verify the flat-key parse grabs the PRODUCE videoPort (mediaPeer),
     * not a consume/other port, and inspect producerId fields. */
    BSDR_INFO("bsdr.cloud", "GET /rooms body (%d B): %.6000s", (int)strlen(body), body);
    /* flat-key search finds the first screen's mediaPeer/mediaServer fields */
    double v;
    if (bsdr_json_get_str(body, "ipAddress", out->media_ip, sizeof(out->media_ip)) &&
        bsdr_json_get_double(body, "videoPort", &v)) {
        out->video_port = (int)v;
        if (bsdr_json_get_double(body, "audioPort", &v)) out->audio_port = (int)v;
        if (bsdr_json_get_double(body, "dataPort",  &v)) out->data_port  = (int)v;
        if (bsdr_json_get_double(body, "micPort",   &v)) out->mic_port   = (int)v;   /* room voice (mono) */
        bsdr_json_get_str(body, "roomId", out->room_id, sizeof(out->room_id));       /* for the room-join mic peer */
        bsdr_json_get_str(body, "userSessionId", out->session_id, sizeof(out->session_id));
        out->found = true;
        BSDR_INFO("bsdr.cloud", "rooms: relay %s video=%d audio=%d mic=%d data=%d session=%s",
                  out->media_ip, out->video_port, out->audio_port, out->mic_port,
                  out->data_port, out->session_id);
        return true;
    }
    BSDR_INFO("bsdr.cloud", "rooms: no shareable screen yet (add one on the Quest)");
    BSDR_DEBUG("bsdr.cloud", "GET /rooms body (%d B): %.1500s", (int)strlen(body), body);
    return true;   /* connected OK, just no screen provisioned yet */
}

bool bsdr_cloud_join_room(const char *access_token, const char *room_id, bsdr_cloud_screen *out) {
    memset(out, 0, sizeof(*out));
    if (!room_id || !room_id[0]) return false;
    /* the roomId carries a "room:" prefix; the REST path + body use the BARE id (a prefixed path 500s,
     * matching the official client's String.Concat of the bare id). */
    const char *bare = strncmp(room_id, "room:", 5) == 0 ? room_id + 5 : room_id;
    char jbody[256];
    int bl = snprintf(jbody, sizeof jbody,
                      "{\"roomId\":\"%s\",\"version\":\"0.950.2\"}", bare);
    char req[4096];
    snprintf(req, sizeof(req),
        "POST /room/%s/join HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "x-access-token: %s\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bare, BSDR_CLOUD_API2_HOST, bsdr_cloud_api_key(), access_token, bl, jbody);
    static char resp[65536];
    int n = https_request(BSDR_CLOUD_API2_HOST, req, resp, sizeof(resp));
    if (n < 0) { BSDR_WARN("bsdr.cloud", "POST /room/%s/join: connection failed", bare); return false; }
    int code = status_code(resp);
    out->http_status = code;
    const char *body = strstr(resp, "\r\n\r\n");
    if (code / 100 != 2 || !body) { BSDR_WARN("bsdr.cloud", "room-join -> HTTP %d", code); return false; }
    body += 4;
    BSDR_INFO("bsdr.cloud", "room-join body (%d B): %.4000s", (int)strlen(body), body);
    /* Same flat-key mediaPeer shape as /rooms, but this peer DOES expose micPort (the room voice). */
    double v;
    if (bsdr_json_get_str(body, "ipAddress", out->media_ip, sizeof(out->media_ip)) &&
        bsdr_json_get_double(body, "micPort", &v)) {
        out->mic_port = (int)v;
        if (bsdr_json_get_double(body, "audioPort", &v)) out->audio_port = (int)v;
        if (bsdr_json_get_double(body, "videoPort", &v)) out->video_port = (int)v;
        if (bsdr_json_get_double(body, "dataPort",  &v)) out->data_port  = (int)v;
        bsdr_json_get_str(body, "userSessionId", out->session_id, sizeof(out->session_id));
        out->found = true;
        BSDR_INFO("bsdr.cloud", "room-join: mic peer %s mic=%d (audio=%d)",
                  out->media_ip, out->mic_port, out->audio_port);
        return true;
    }
    BSDR_WARN("bsdr.cloud", "room-join: no micPort in the response");
    return false;
}

bool bsdr_cloud_get_room(const char *access_token, const char *room_id, bsdr_cloud_screen *out) {
    memset(out, 0, sizeof(*out));
    if (!room_id || !room_id[0]) return false;
    /* GET /room/{id} with the FULL room id, URL-encoded (only ':' needs escaping in a Bigscreen id).
     * This mirrors bsandroid's cloud.getRoom(enc(roomId)). */
    char enc[192]; size_t eo = 0;
    for (const char *p = room_id; *p && eo < sizeof enc - 4; p++) {
        if (*p == ':') { enc[eo++] = '%'; enc[eo++] = '3'; enc[eo++] = 'A'; }
        else enc[eo++] = *p;
    }
    enc[eo] = 0;
    char req[4096];
    snprintf(req, sizeof(req),
        "GET /room/%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "x-access-token: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n",
        enc, BSDR_CLOUD_API2_HOST, bsdr_cloud_api_key(), access_token);
    static char resp[131072];
    int n = https_request(BSDR_CLOUD_API2_HOST, req, resp, sizeof(resp));
    if (n < 0) { BSDR_WARN("bsdr.cloud", "GET /room: connection failed"); return false; }
    int code = status_code(resp);
    out->http_status = code;
    const char *body = strstr(resp, "\r\n\r\n");
    if (code / 100 != 2 || !body) { BSDR_WARN("bsdr.cloud", "GET /room -> HTTP %d", code); return false; }
    body += 4;
    BSDR_INFO("bsdr.cloud", "GET /room body (%d B): %.4000s", (int)strlen(body), body);
    /* The room-voice micPort is on OUR peer: scan from "localUser" so the flat key-search grabs that
     * mediaPeer's ipAddress + micPort (the screen peer earlier in the body has no micPort). */
    const char *scan = strstr(body, "\"localUser\"");
    if (!scan) scan = body;
    double v;
    if (bsdr_json_get_str(scan, "ipAddress", out->media_ip, sizeof(out->media_ip)) &&
        bsdr_json_get_double(scan, "micPort", &v) && (int)v > 0) {
        out->mic_port = (int)v;
        if (bsdr_json_get_double(scan, "audioPort", &v)) out->audio_port = (int)v;
        if (bsdr_json_get_double(scan, "dataPort",  &v)) out->data_port  = (int)v;
        bsdr_json_get_str(scan, "userSessionId", out->session_id, sizeof(out->session_id));
        out->found = true;
        BSDR_INFO("bsdr.cloud", "GET /room: mic peer %s mic=%d", out->media_ip, out->mic_port);
        return true;
    }
    BSDR_WARN("bsdr.cloud", "GET /room: no localUser micPort");
    return false;
}

/* ---- WS presence ---- */
#include "bsdr/platform.h"
struct bsdr_cloud_ws { SSL_CTX *ctx; BIO *bio; bsdr_thread *thr; volatile int stop; };
/* Minimal MessagePack -> JSON-ish pretty-printer. Appends one MP object at p into (out,*ow)
 * bounded by outcap; returns bytes consumed from p, or -1 on truncation/error. Enough to read
 * the room signaling (maps/arrays/str/int/float/bool/nil/bin). */
static int mp_pp(const unsigned char *p, size_t len, char *out, size_t outcap, int *ow, int depth) {
#define MPO(...) do { if (*ow < (int)outcap - 1) *ow += snprintf(out + *ow, outcap - (size_t)*ow, __VA_ARGS__); } while (0)
    /* Bound the recursion: a hostile WS frame of nested fixarray/fixmap bytes (0x9x/0x8x) is one
     * nesting level per byte, so a 64 KB frame could blow the stack. 64 levels is far beyond any
     * real room-signaling object. */
    if (depth > 64) return -1;
    if (len < 1) return -1;
    unsigned c = p[0];
    if (c <= 0x7f) { MPO("%u", c); return 1; }
    if (c >= 0xe0) { MPO("%d", (int)(signed char)c); return 1; }
    if (c >= 0xa0 && c <= 0xbf) { size_t n = c & 0x1f; if (1 + n > len) return -1; MPO("\"%.*s\"", (int)n, p + 1); return (int)(1 + n); }
    if (c >= 0x90 && c <= 0x9f) { int n = c & 0x0f, off = 1; MPO("["); for (int i = 0; i < n; i++) { if (i) MPO(","); int u = mp_pp(p + off, len - off, out, outcap, ow, depth + 1); if (u < 0) return -1; off += u; } MPO("]"); return off; }
    if (c >= 0x80 && c <= 0x8f) { int n = c & 0x0f, off = 1; MPO("{"); for (int i = 0; i < n; i++) { if (i) MPO(","); int u = mp_pp(p + off, len - off, out, outcap, ow, depth + 1); if (u < 0) return -1; off += u; MPO(":"); u = mp_pp(p + off, len - off, out, outcap, ow, depth + 1); if (u < 0) return -1; off += u; } MPO("}"); return off; }
    switch (c) {
        case 0xc0: MPO("null"); return 1;
        case 0xc2: MPO("false"); return 1;
        case 0xc3: MPO("true"); return 1;
        case 0xcc: if (len < 2) return -1; MPO("%u", p[1]); return 2;
        case 0xcd: if (len < 3) return -1; MPO("%u", (p[1] << 8) | p[2]); return 3;
        case 0xce: if (len < 5) return -1; MPO("%u", ((unsigned)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]); return 5;
        case 0xcf: if (len < 9) return -1; { unsigned long long v = 0; for (int i = 1; i <= 8; i++) v = (v << 8) | p[i]; MPO("%llu", v); } return 9;
        case 0xd0: if (len < 2) return -1; MPO("%d", (int)(signed char)p[1]); return 2;
        case 0xd1: if (len < 3) return -1; MPO("%d", (short)((p[1] << 8) | p[2])); return 3;
        case 0xd2: if (len < 5) return -1; MPO("%d", (int)(((unsigned)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4])); return 5;
        case 0xd3: if (len < 9) return -1; { long long v = 0; for (int i = 1; i <= 8; i++) v = (v << 8) | p[i]; MPO("%lld", v); } return 9;
        case 0xca: if (len < 5) return -1; { unsigned u = ((unsigned)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]; float f; memcpy(&f, &u, 4); MPO("%g", (double)f); } return 5;
        case 0xcb: if (len < 9) return -1; { unsigned long long u = 0; for (int i = 1; i <= 8; i++) u = (u << 8) | p[i]; double d; memcpy(&d, &u, 8); MPO("%g", d); } return 9;
        case 0xd9: { if (len < 2) return -1; size_t n = p[1]; if (2 + n > len) return -1; MPO("\"%.*s\"", (int)n, p + 2); return (int)(2 + n); }
        case 0xda: { if (len < 3) return -1; size_t n = ((size_t)p[1] << 8) | p[2]; if (3 + n > len) return -1; MPO("\"%.*s\"", (int)n, p + 3); return (int)(3 + n); }
        case 0xdb: { if (len < 5) return -1; size_t n = ((size_t)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]; if (5 + n > len) return -1; MPO("\"%.*s\"", (int)n, p + 5); return (int)(5 + n); }
        case 0xc4: { if (len < 2) return -1; size_t n = p[1]; if (2 + n > len) return -1; MPO("<bin %zu>", n); return (int)(2 + n); }
        case 0xc5: { if (len < 3) return -1; size_t n = ((size_t)p[1] << 8) | p[2]; if (3 + n > len) return -1; MPO("<bin %zu>", n); return (int)(3 + n); }
        case 0xdc: { if (len < 3) return -1; int n = (p[1] << 8) | p[2], off = 3; MPO("["); for (int i = 0; i < n; i++) { if (i) MPO(","); int u = mp_pp(p + off, len - off, out, outcap, ow, depth + 1); if (u < 0) return -1; off += u; } MPO("]"); return off; }
        case 0xde: { if (len < 3) return -1; int n = (p[1] << 8) | p[2], off = 3; MPO("{"); for (int i = 0; i < n; i++) { if (i) MPO(","); int u = mp_pp(p + off, len - off, out, outcap, ow, depth + 1); if (u < 0) return -1; off += u; MPO(":"); u = mp_pp(p + off, len - off, out, outcap, ow, depth + 1); if (u < 0) return -1; off += u; } MPO("}"); return off; }
    }
    MPO("<0x%02x?>", c); return 1;
#undef MPO
}

/* Dump one WS data frame (both to the log and, if BSDR_DUMP_WS=<file>, appended there). */
static void ws_dump_frame(const char *dir, int op, const unsigned char *pl, size_t len) {
    static FILE *fp = NULL; static int init = 0;
    if (!init) { init = 1; const char *d = getenv("BSDR_DUMP_WS"); if (d) fp = fopen(d, "a"); }
    char txt[8192]; int ow = 0; txt[0] = 0;
    if (op == 0x2 && len > 0) { mp_pp(pl, len, txt, sizeof(txt), &ow, 0); }        /* MessagePack */
    else if (op == 0x1 || op == 0x0) { size_t n = len < sizeof(txt) - 1 ? len : sizeof(txt) - 1; memcpy(txt, pl, n); txt[n] = 0; }
    if (fp) {
        fprintf(fp, "%s op=%d len=%zu  %s\n  hex: ", dir, op, len, txt);
        for (size_t i = 0; i < len && i < 2048; i++) fprintf(fp, "%02x", pl[i]);
        fprintf(fp, "%s\n", len > 2048 ? "..." : "");
        fflush(fp);
    }
    /* wired into the debug log by default (tag bsdr.ws): full MessagePack/text decode + a hex
     * preview so every room frame is inspectable straight from debug.log, no env var needed.
     * The logger streams to stderr (no fixed buffer), so long frames aren't truncated. */
    char hex[256 * 2 + 4]; int hn = 0; size_t hl = len < 256 ? len : 256;
    for (size_t i = 0; i < hl; i++) hn += snprintf(hex + hn, sizeof(hex) - (size_t)hn, "%02x", pl[i]);
    BSDR_INFO("bsdr.ws", "%s op=%d %zuB: %s | hex[%zu]: %s%s", dir, op, len,
              txt[0] ? txt : "(non-text)", hl, hex, len > 256 ? "..." : "");
}

/* Reassemble the TLS byte stream into WS frames and dump every one — full room protocol. */
static void ws_keepalive(void *arg) {
    struct bsdr_cloud_ws *w = (struct bsdr_cloud_ws *)arg;
    size_t cap = 65536, blen = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return;
    while (!w->stop) {
        if (blen == cap) { size_t nc = cap * 2; unsigned char *nb = realloc(buf, nc); if (!nb) break; buf = nb; cap = nc; }
        int n = BIO_read(w->bio, buf + blen, (int)(cap - blen));
        if (n <= 0) { if (BIO_should_retry(w->bio)) { bsdr_sleep_ms(150); continue; } break; }
        blen += (size_t)n;
        size_t off = 0;
        while (blen - off >= 2) {                       /* extract every complete frame */
            unsigned char *f = buf + off;
            int op = f[0] & 0x0f, masked = f[1] & 0x80;
            size_t hl = 2, len = f[1] & 0x7f;
            if (len == 126) { if (blen - off < 4) break; len = ((size_t)f[2] << 8) | f[3]; hl = 4; }
            else if (len == 127) { if (blen - off < 10) break; len = 0; for (int i = 2; i < 10; i++) len = (len << 8) | f[i]; hl = 10; }
            size_t ml = masked ? 4u : 0u;
            if (blen - off < hl + ml + len) break;      /* frame not fully arrived yet */
            unsigned char *pl = f + hl + ml;
            if (masked) for (size_t i = 0; i < len; i++) pl[i] ^= f[hl + (i & 3)];
            if (op == 0x9) { unsigned char pong[6] = { 0x8a, 0x80, 0, 0, 0, 0 }; BIO_write(w->bio, pong, 6); }
            else if (op == 0x8) { BSDR_WARN("bsdr.cloud", "WS: server closed the presence socket"); w->stop = 1; }
            else if (op == 0x0 || op == 0x1 || op == 0x2) ws_dump_frame("RX", op, pl, len);
            off += hl + ml + len;
        }
        if (off > 0) { memmove(buf, buf + off, blen - off); blen -= off; }
    }
    free(buf);
}
bsdr_cloud_ws *bsdr_cloud_ws_open(const char *access_token) {
    char cs[8192];
    make_connection_string(access_token, cs, sizeof(cs));
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    bsdr_tls_configure_client(ctx, BSDR_CLOUD_WS_HOST);
    BIO *bio = BIO_new_ssl_connect(ctx);
    SSL *ssl = NULL; BIO_get_ssl(bio, &ssl);
    if (!ssl) { BIO_free_all(bio); SSL_CTX_free(ctx); return NULL; }
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
    SSL_set_tlsext_host_name(ssl, BSDR_CLOUD_WS_HOST);
    char hp[300]; snprintf(hp, sizeof(hp), "%s:443", BSDR_CLOUD_WS_HOST);
    BIO_set_conn_hostname(bio, hp);
    if (BIO_do_connect(bio) <= 0) { BSDR_ERROR("bsdr.cloud", "WS connect failed"); goto fail; }
    char hs[9216];
    int hl = snprintf(hs, sizeof(hs),
        "GET /%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
        cs, BSDR_CLOUD_WS_HOST);
    if (BIO_write(bio, hs, hl) <= 0) goto fail;
    char resp[2048]; int rn = BIO_read(bio, resp, sizeof(resp) - 1);
    if (rn <= 0) goto fail;
    resp[rn] = '\0';
    if (!strstr(resp, " 101")) { BSDR_WARN("bsdr.cloud", "WS upgrade rejected: %.40s", resp); goto fail; }
    struct bsdr_cloud_ws *w = calloc(1, sizeof(*w));
    if (!w) goto fail;
    w->ctx = ctx; w->bio = bio;
    w->thr = bsdr_thread_start(ws_keepalive, w);
    BSDR_INFO("bsdr.cloud", "WS presence connected to %s (host online)", BSDR_CLOUD_WS_HOST);
    return w;
fail:
    BIO_free_all(bio); SSL_CTX_free(ctx); return NULL;
}
void bsdr_cloud_ws_close(bsdr_cloud_ws *w) {
    if (!w) return;
    w->stop = 1;
    if (w->thr) bsdr_thread_join(w->thr);
    BIO_free_all(w->bio); SSL_CTX_free(w->ctx); free(w);
}
