/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
#include "bsdr/browserctl.h"
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"   /* portable sockets: winsock2/ws2tcpip on Windows, BSD sockets on POSIX */
#include "bsdr/net.h"        /* bsdr_socket_close (closesocket on Windows) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>       /* ssize_t on mingw */
#include <errno.h>
#if defined(_WIN32)
#  include <ws2tcpip.h>      /* getaddrinfo/freeaddrinfo (also via platform.h) */
#else
#  include <unistd.h>
#  include <netdb.h>
#  include <sys/select.h>
#endif

/* --- tiny URL parse: "http://host:port[/path]" -> host, port ------------------------------------ */
static bool parse_endpoint(const char *ep, char *host, size_t hcap, int *port) {
    if (!ep || !host || !port) return false;
    const char *p = ep;
    if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "https://", 8)) p += 8;   /* CDP is http, but tolerate a stray scheme */
    size_t n = 0;
    *port = 9222;
    while (*p && *p != ':' && *p != '/' && n + 1 < hcap) host[n++] = *p++;
    host[n] = '\0';
    if (!host[0]) return false;
    if (*p == ':') { *port = atoi(p + 1); if (*port <= 0) *port = 9222; }
    return true;
}

/* --- blocking TCP connect (localhost debug endpoint) --------------------------------------------- */
static bsdr_socket_t tcp_connect(const char *host, int port) {
    char pstr[16]; snprintf(pstr, sizeof pstr, "%d", port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, pstr, &hints, &res) != 0) return BSDR_INVALID_SOCKET;
    bsdr_socket_t fd = BSDR_INVALID_SOCKET;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == BSDR_INVALID_SOCKET) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        bsdr_socket_close(fd); fd = BSDR_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return fd;
}

static bool read_line(bsdr_socket_t fd, char *out, size_t cap) {
    size_t o = 0;
    while (o + 1 < cap) {
        char ch; ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0) return false;
        out[o++] = ch;
        if (ch == '\n') break;
    }
    out[o] = '\0';
    return true;
}

/* WebSocket upgrade on `path`. A constant key is fine for a localhost debug socket (Chrome doesn't
 * verify the client key beyond format). Returns true on HTTP 101. */
static bool ws_handshake(bsdr_socket_t fd, const char *host, int port, const char *path) {
    char req[1024];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
        path[0] ? path : "/", host, port);
    if (n <= 0 || send(fd, req, (size_t)n, 0) != n) return false;
    char line[512];
    if (!read_line(fd, line, sizeof line)) return false;
    bool ok = strstr(line, " 101") != NULL;
    /* drain the rest of the response headers */
    while (read_line(fd, line, sizeof line)) if (!strcmp(line, "\r\n") || !strcmp(line, "\n")) break;
    return ok;
}

/* Send one masked text frame (client->server frames MUST be masked). */
static bool ws_send_text(bsdr_socket_t fd, const char *msg) {
    size_t len = strlen(msg);
    unsigned char hdr[14]; size_t ho = 0;
    hdr[ho++] = 0x81;                                  /* FIN + text opcode */
    unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    if (len < 126)        hdr[ho++] = 0x80 | (unsigned char)len;
    else if (len < 65536) { hdr[ho++] = 0x80 | 126; hdr[ho++] = (len >> 8) & 0xff; hdr[ho++] = len & 0xff; }
    else { hdr[ho++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[ho++] = (unsigned char)((uint64_t)len >> (i * 8)); }
    memcpy(hdr + ho, mask, 4); ho += 4;
    if (send(fd, (const char *)hdr, ho, 0) != (ssize_t)ho) return false;   /* winsock send() takes char* */
    /* mask + send the payload in a scratch buffer */
    char *buf = malloc(len ? len : 1);
    if (!buf) return false;
    for (size_t i = 0; i < len; i++) buf[i] = (char)((unsigned char)msg[i] ^ mask[i & 3]);
    bool ok = send(fd, buf, len, 0) == (ssize_t)len;
    free(buf);
    return ok;
}

static bool recv_all(bsdr_socket_t fd, unsigned char *buf, size_t len, int timeout_ms) {
    size_t got = 0;
    while (got < len) {
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int s = select((int)(fd + 1), &rf, NULL, NULL, &tv);   /* nfds ignored on Windows */
        if (s <= 0) return false;
        ssize_t r = recv(fd, (char *)(buf + got), len - got, 0);   /* winsock recv() takes char* */
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* Receive one text frame (server frames are unmasked). Skips ping/other control frames. */
static int ws_recv_text(bsdr_socket_t fd, char *out, size_t cap, int timeout_ms) {
    for (int guard = 0; guard < 64; guard++) {
        unsigned char h[2];
        if (!recv_all(fd, h, 2, timeout_ms)) return -1;
        int opcode = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) { unsigned char e[2]; if (!recv_all(fd, e, 2, timeout_ms)) return -1; len = (e[0] << 8) | e[1]; }
        else if (len == 127) { unsigned char e[8]; if (!recv_all(fd, e, 8, timeout_ms)) return -1; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
        if (h[1] & 0x80) { unsigned char mk[4]; if (!recv_all(fd, mk, 4, timeout_ms)) return -1; }  /* shouldn't happen server->client */
        unsigned char *pl = malloc(len ? len : 1);
        if (!pl) return -1;
        if (!recv_all(fd, pl, len, timeout_ms)) { free(pl); return -1; }
        if (opcode == 0x1) {   /* text */
            size_t c = len < cap - 1 ? len : cap - 1;
            memcpy(out, pl, c); out[c] = '\0';
            free(pl);
            return (int)c;
        }
        free(pl);
        if (opcode == 0x8) return -1;   /* close */
        /* ping/pong/continuation: ignore and read the next frame */
    }
    return -1;
}

/* Run one CDP command: discover the page WS, connect, send {id,method,params}, return the raw JSON
 * response text (the caller extracts what it needs). Returns 0 on success. */
static int cdp_call(const char *endpoint, const char *method, const char *params_json,
                    char *resp_out, size_t resp_cap) {
    char host[128]; int port;
    if (!parse_endpoint(endpoint, host, sizeof host, &port)) {
        snprintf(resp_out, resp_cap, "invalid CDP endpoint");
        return -1;
    }
    /* 1) list targets, pick the first "page" webSocketDebuggerUrl */
    char listurl[192]; snprintf(listurl, sizeof listurl, "http://%s:%d/json", host, port);
    static char http[64 * 1024];
    int r = bsdr_http_request("GET", listurl, NULL, 0, NULL, NULL, 0, http, sizeof http);
    if (r < 0 || bsdr_http_status(http) / 100 != 2) {
        snprintf(resp_out, resp_cap, "cannot reach the browser debug endpoint at %s:%d (start it with --remote-debugging-port=%d)", host, port, port);
        return -1;
    }
    const char *body = bsdr_http_body(http);
    /* find a "page" target's webSocketDebuggerUrl (fall back to the first ws url) */
    char wsurl[256] = "";
    const char *p = body;
    while (p && (p = strstr(p, "webSocketDebuggerUrl")) != NULL) {
        char cand[256] = "";
        bsdr_json_get_str(p - 1, "webSocketDebuggerUrl", cand, sizeof cand);
        /* prefer a page target: check the surrounding object for "type":"page" */
        const char *obj = p;
        for (int b = 0; b < 400 && obj > body; b++, obj--) if (*obj == '{') break;
        if (cand[0]) {
            if (!wsurl[0]) snprintf(wsurl, sizeof wsurl, "%s", cand);
            if (strstr(obj, "\"type\":\"page\"") || strstr(obj, "\"type\": \"page\"")) {
                snprintf(wsurl, sizeof wsurl, "%s", cand);
                break;
            }
        }
        p += 20;
    }
    if (!wsurl[0]) { snprintf(resp_out, resp_cap, "no debuggable page found (open a tab in the browser)"); return -1; }
    /* ws url is ws://host:port/devtools/page/ID — extract the path */
    const char *path = strstr(wsurl, "://");
    path = path ? strchr(path + 3, '/') : NULL;
    if (!path) { snprintf(resp_out, resp_cap, "malformed ws url"); return -1; }

    /* 2) connect + upgrade */
    bsdr_socket_t fd = tcp_connect(host, port);
    if (fd == BSDR_INVALID_SOCKET) { snprintf(resp_out, resp_cap, "connect failed"); return -1; }
    int rc = -1;
    if (!ws_handshake(fd, host, port, path)) { snprintf(resp_out, resp_cap, "websocket upgrade failed"); goto out; }

    /* 3) send the command */
    { char cmd[4096];
      snprintf(cmd, sizeof cmd, "{\"id\":1,\"method\":\"%s\",\"params\":%s}", method, params_json ? params_json : "{}");
      if (!ws_send_text(fd, cmd)) { snprintf(resp_out, resp_cap, "send failed"); goto out; } }

    /* 4) read frames until we see our id:1 (CDP may interleave events) */
    for (int i = 0; i < 32; i++) {
        int n = ws_recv_text(fd, resp_out, resp_cap, 8000);
        if (n < 0) { snprintf(resp_out, resp_cap, "no reply from the browser"); goto out; }
        if (strstr(resp_out, "\"id\":1") || strstr(resp_out, "\"id\": 1")) { rc = 0; break; }
        /* else it's an event; keep reading */
    }
out:
    bsdr_socket_close(fd);
    return rc;
}

int bsdr_browser_navigate(const char *http_endpoint, const char *url, char *result, size_t cap) {
    if (!url || !url[0]) { snprintf(result, cap, "no url"); return -1; }
    char params[1200], eu[1024];
    bsdr_json_escape(eu, sizeof eu, url);
    snprintf(params, sizeof params, "{\"url\":\"%s\"}", eu);
    static char resp[64 * 1024];
    int rc = cdp_call(http_endpoint, "Page.navigate", params, resp, sizeof resp);
    if (rc != 0) { snprintf(result, cap, "%s", resp); return rc; }
    if (strstr(resp, "\"errorText\"")) { char e[256] = ""; bsdr_json_get_str(resp, "errorText", e, sizeof e);
        snprintf(result, cap, "navigation error: %s", e[0] ? e : "failed"); return -1; }
    snprintf(result, cap, "navigated to %s", url);
    return 0;
}

int bsdr_browser_eval(const char *http_endpoint, const char *expr, char *result, size_t cap) {
    if (!expr || !expr[0]) { snprintf(result, cap, "no expression"); return -1; }
    char params[4096], ee[3000];
    bsdr_json_escape(ee, sizeof ee, expr);
    /* returnByValue so we get a JSON value back; await so promises resolve */
    snprintf(params, sizeof params,
        "{\"expression\":\"%s\",\"returnByValue\":true,\"awaitPromise\":true,\"userGesture\":true}", ee);
    static char resp[64 * 1024];
    int rc = cdp_call(http_endpoint, "Runtime.evaluate", params, resp, sizeof resp);
    if (rc != 0) { snprintf(result, cap, "%s", resp); return rc; }
    if (strstr(resp, "\"exceptionDetails\"")) {
        char e[512] = ""; bsdr_json_get_str(resp, "description", e, sizeof e);
        snprintf(result, cap, "JS error: %s", e[0] ? e : "exception");
        return -1;
    }
    /* pull result.result.value (may be string/number/bool/object). Just return the "value" text. */
    const char *rp = strstr(resp, "\"result\"");
    char val[4096] = "";
    if (rp && bsdr_json_get_str(rp, "value", val, sizeof val) && val[0]) snprintf(result, cap, "%s", val);
    else snprintf(result, cap, "ok");
    return 0;
}
