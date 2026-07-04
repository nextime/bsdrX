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
#include "bsdr/control.h"
#include "bsdr/protocol.h"
#include "bsdr/net.h"
#include "bsdr/json.h"
#include "bsdr/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* portable case-insensitive substring search (strcasestr is non-portable) */
static const char *ci_find(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return p;
    }
    return NULL;
}

struct bsdr_control {
    char pairing_code[8];
    bsdr_control_cbs cbs;
    bsdr_socket_t listener;
    bsdr_thread *thread;
    bsdr_mutex *lock;
    bool have_device;
    bsdr_paired_device device;
    int pair_fails;            /* consecutive wrong pairing-code attempts (brute-force guard) */
    time_t pair_lock_until;    /* reject /pair until this time after too many wrong codes */
    volatile int running;
};

#define BSDR_PAIR_MAX_FAILS   5     /* wrong codes before a cooldown kicks in */
#define BSDR_PAIR_COOLDOWN_S  30    /* seconds /pair is blocked after MAX_FAILS */

/* -------------------------------------------------------------- csprng utils */
void bsdr_gen_hex(char *out, size_t nbytes) {
    unsigned char buf[64];
    if (nbytes > sizeof(buf)) nbytes = sizeof(buf);
    bsdr_random_bytes(buf, nbytes);
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < nbytes; i++) {
        out[2 * i] = hex[buf[i] >> 4];
        out[2 * i + 1] = hex[buf[i] & 0xF];
    }
    out[2 * nbytes] = '\0';
}

void bsdr_gen_pairing_code(char *out6) {
    unsigned char buf[6];
    bsdr_random_bytes(buf, sizeof(buf));
    for (int i = 0; i < 6; i++) out6[i] = (char)('0' + (buf[i] % 10));
    out6[6] = '\0';
}

/* ------------------------------------------------------------- HTTP plumbing */
typedef struct {
    char method[8];               /* per-request (no static: thread-per-conn) */
    char path[256];
    const char *body;             /* points into the per-connection buffer */
    bool keep_alive;              /* client wants the connection reused */
} http_req;

static void send_response_ka(bsdr_socket_t c, bool keep_alive, int status,
                             const char *status_text, const char *ctype,
                             const char *body) {
    char hdr[512];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: %s\r\n\r\n",
        status, status_text, ctype ? ctype : "text/plain", blen,
        keep_alive ? "keep-alive" : "close");
    bsdr_send_all(c, hdr, (size_t)n);
    if (blen) bsdr_send_all(c, body, blen);
}

/* split "/start/<id>" -> route="/start", id="<id>" (id may be empty). */
static void split_path(const char *path, char *route, size_t rlen,
                       char *id, size_t ilen) {
    const char *p = path + 1;            /* skip leading '/' */
    const char *slash = strchr(p, '/');
    if (!slash) {
        snprintf(route, rlen, "%.*s", (int)rlen - 1, path);
        if (ilen) id[0] = '\0';
        return;
    }
    size_t seg = (size_t)(slash - path);
    if (seg >= rlen) seg = rlen - 1;
    memcpy(route, path, seg);
    route[seg] = '\0';
    snprintf(id, ilen, "%s", slash + 1);
}

/* --------------------------------------------------------------- handlers ---*/
static bool check_paired(struct bsdr_control *c, const char *id,
                         bsdr_paired_device *copy) {
    bool ok = c->have_device && strcmp(c->device.pairing_id, id) == 0;
    if (ok && copy) *copy = c->device;
    return ok;
}

static void handle_request(struct bsdr_control *c, bsdr_socket_t conn,
                           const http_req *req, const char *remote_ip) {
    char route[64], id[160];
    split_path(req->path, route, sizeof(route), id, sizeof(id));
    BSDR_DEBUG("bsdr.control", "%s %s from %s", req->method, req->path, remote_ip);

    /* POST /pair */
    if (strcmp(req->method, "POST") == 0 && strcmp(route, "/pair") == 0) {
        char code[32] = {0};
        bsdr_mutex_lock(c->lock);
        /* Brute-force guard: the pairing code is only 6 digits (10^6). Without a throttle a LAN
         * attacker could parallelize guesses across connections. After a few wrong codes, block
         * /pair for a cooldown so guessing the code before the operator notices is infeasible. */
        time_t now = time(NULL);
        if (c->pair_lock_until > now) {
            bsdr_mutex_unlock(c->lock);
            BSDR_WARN("bsdr.control", "pair from %s throttled (%ld s left)",
                      remote_ip, (long)(c->pair_lock_until - now));
            send_response_ka(conn, req->keep_alive, 429, "Too Many Requests", "text/plain",
                             "Too many attempts; try again later.");
            return;
        }
        if (!bsdr_json_get_str(req->body, "pairingRequestCode", code, sizeof(code)) ||
            strcmp(code, c->pairing_code) != 0) {
            if (++c->pair_fails >= BSDR_PAIR_MAX_FAILS) {
                c->pair_lock_until = now + BSDR_PAIR_COOLDOWN_S;
                c->pair_fails = 0;
            }
            bsdr_mutex_unlock(c->lock);
            BSDR_DEBUG("bsdr.control", "pair rejected from %s: code '%s' != '%s'",
                       remote_ip, code, c->pairing_code);
            send_response_ka(conn, req->keep_alive, 403, "Forbidden", "text/plain", "Code mismatch");
            return;
        }
        c->pair_fails = 0;   /* correct code — clear the guard */
        if (c->have_device) {
            BSDR_DEBUG("bsdr.control", "pair rejected from %s: already paired with %s (%s)",
                       remote_ip, c->device.device_name, c->device.remote_ip);
            bsdr_mutex_unlock(c->lock);
            send_response_ka(conn, req->keep_alive, 403, "Forbidden", "text/plain", "Already paired.");
            return;
        }
        if (c->cbs.allow_pair && !c->cbs.allow_pair(remote_ip, c->cbs.user)) {
            bsdr_mutex_unlock(c->lock);
            BSDR_DEBUG("bsdr.control", "pair rejected from %s: not the selected headset", remote_ip);
            send_response_ka(conn, req->keep_alive, 403, "Forbidden", "text/plain", "Not the selected headset.");
            return;
        }
        memset(&c->device, 0, sizeof(c->device));
        bsdr_gen_hex(c->device.pairing_id, 48);
        bsdr_json_get_str(req->body, "deviceId", c->device.device_id,
                          sizeof(c->device.device_id));
        bsdr_json_get_str(req->body, "deviceName", c->device.device_name,
                          sizeof(c->device.device_name));
        snprintf(c->device.remote_ip, sizeof(c->device.remote_ip), "%.63s", remote_ip);
        c->device.created_at = c->device.last_keepalive = time(NULL);
        c->have_device = true;
        char body[160];
        snprintf(body, sizeof(body), "{\"pairingId\":\"%s\"}", c->device.pairing_id);
        BSDR_INFO("bsdr.control", "paired with %s (%.8s) from %s",
                  c->device.device_name, c->device.pairing_id, remote_ip);
        bsdr_mutex_unlock(c->lock);
        send_response_ka(conn, req->keep_alive, 200, "OK", "application/json", body);
        return;
    }

    /* everything else needs a valid pairing id */
    bsdr_paired_device snapshot;
    bsdr_mutex_lock(c->lock);
    if (!check_paired(c, id, &snapshot)) {
        bool none = !c->have_device;
        bsdr_mutex_unlock(c->lock);
        BSDR_DEBUG("bsdr.control", "%s %s from %s rejected: %s", req->method, route, remote_ip,
                   none ? "not paired yet" : "pairing id mismatch (stale/duplicate session?)");
        if (none) send_response_ka(conn, req->keep_alive, 404, "Not Found", "text/plain", "Need to pair first.");
        else      send_response_ka(conn, req->keep_alive, 403, "Forbidden", "text/plain", "Pairing id was wrong.");
        return;
    }

    if (strcmp(req->method, "GET") == 0 && strcmp(route, "/heartbeat") == 0) {
        BSDR_DEBUG("bsdr.control", "heartbeat from %s (idle %.0fms)", remote_ip,
                   difftime(time(NULL), c->device.last_keepalive) * 1000.0);
        c->device.last_keepalive = time(NULL);
        bsdr_mutex_unlock(c->lock);
        send_response_ka(conn, req->keep_alive, 200, "OK", "text/plain", "OK");
        return;
    }
    if (strcmp(req->method, "GET") == 0 && strcmp(route, "/start") == 0) {
        c->device.is_sharing = true;
        snapshot = c->device;
        bsdr_mutex_unlock(c->lock);
        BSDR_INFO("bsdr.control", "start sharing -> %s", snapshot.remote_ip);
        if (c->cbs.on_start) c->cbs.on_start(&snapshot, c->cbs.user);
        send_response_ka(conn, req->keep_alive, 200, "OK", "text/plain", "OK");
        return;
    }
    if (strcmp(req->method, "PUT") == 0 && strcmp(route, "/device") == 0) {
        if (!c->device.is_sharing) {
            bsdr_mutex_unlock(c->lock);
            BSDR_DEBUG("bsdr.control", "settings PUT from %s rejected: not sharing yet", remote_ip);
            send_response_ka(conn, req->keep_alive, 403, "Forbidden", "text/plain",
                          "Not sharing. Start streaming first.");
            return;
        }
        BSDR_DEBUG("bsdr.control", "/device body from %s: %.300s", remote_ip,
                   req->body ? req->body : "(none)");
        double v;
        if (bsdr_json_get_double(req->body, "bitrate", &v)) c->device.bitrate = (long)v;
        else if (bsdr_json_get_double(req->body, "fec", &v)) c->device.fec = (long)v;
        else if (bsdr_json_get_double(req->body, "fps", &v)) c->device.fps = (long)v;
        else if (bsdr_json_get_double(req->body, "resolution", &v)) c->device.resolution = (long)v;
        /* The official client toggles internet sharing here: {"isInternetSharing":true|false}.
         * Parse it independently of the one-field-per-PUT tunables above. (No bool JSON getter,
         * so scan the value: whichever of true/false follows the key first.) */
        c->device.internet_sharing = -1;
        {
            const char *p = req->body ? strstr(req->body, "isInternetSharing") : NULL;
            if (p) {
                const char *t = strstr(p, "true"), *f = strstr(p, "false");
                c->device.internet_sharing = (t && (!f || t < f)) ? 1 : 0;
            }
        }
        BSDR_DEBUG("bsdr.control", "settings from %s: bitrate=%ld fec=%ld fps=%ld resolution=%ld share=%d",
                   remote_ip, c->device.bitrate, c->device.fec, c->device.fps, c->device.resolution,
                   c->device.internet_sharing);
        snapshot = c->device;
        bsdr_mutex_unlock(c->lock);
        if (c->cbs.on_settings) c->cbs.on_settings(&snapshot, c->cbs.user);
        send_response_ka(conn, req->keep_alive, 200, "OK", "text/plain", "OK");
        return;
    }
    if (strcmp(req->method, "GET") == 0 && strcmp(route, "/stop") == 0) {
        c->device.is_sharing = false;
        snapshot = c->device;
        bsdr_mutex_unlock(c->lock);
        BSDR_INFO("bsdr.control", "stop sharing");
        if (c->cbs.on_stop) c->cbs.on_stop(&snapshot, c->cbs.user);
        send_response_ka(conn, req->keep_alive, 200, "OK", "text/plain", "OK");
        return;
    }
    if (strcmp(req->method, "GET") == 0 && strcmp(route, "/unpair") == 0) {
        snapshot = c->device;
        c->have_device = false;
        bsdr_mutex_unlock(c->lock);
        BSDR_INFO("bsdr.control", "unpair %s", snapshot.device_name);
        if (c->cbs.on_unpair) c->cbs.on_unpair(&snapshot, c->cbs.user);
        send_response_ka(conn, req->keep_alive, 200, "OK", "text/plain", "OK");
        return;
    }

    bsdr_mutex_unlock(c->lock);
    send_response_ka(conn, req->keep_alive, 404, "Not Found", "text/plain", "unknown");
}

/* read a full request (headers + Content-Length body) into buf */
static bool read_request(bsdr_socket_t conn, char *buf, size_t cap, http_req *req) {
    size_t total = 0;
    char *hdr_end = NULL;
    while (total < cap - 1) {
        int n = (int)recv(conn, buf + total, (int)(cap - 1 - total), 0);
        if (n <= 0) return false;
        total += (size_t)n;
        buf[total] = '\0';
        if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
    }
    if (!hdr_end) return false;

    /* parse request line: METHOD SP PATH SP HTTP/x */
    char *sp1 = strchr(buf, ' ');
    if (!sp1) return false;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen >= sizeof(req->method)) mlen = sizeof(req->method) - 1;
    memcpy(req->method, buf, mlen); req->method[mlen] = '\0';
    char *path = sp1 + 1;
    char *sp2 = strchr(path, ' ');
    if (!sp2) return false;
    size_t plen = (size_t)(sp2 - path);
    if (plen >= sizeof(req->path)) plen = sizeof(req->path) - 1;
    memcpy(req->path, path, plen); req->path[plen] = '\0';

    /* keep-alive: HTTP/1.1 defaults to keep-alive unless "Connection: close" */
    {
        const char *vstart = sp2 + 1;            /* "HTTP/1.x" */
        bool http11 = strncmp(vstart, "HTTP/1.1", 8) == 0;
        const char *conn_hdr = ci_find(buf, "Connection:");
        req->keep_alive = http11;
        if (conn_hdr) {
            const char *v = conn_hdr + 11;
            while (*v == ' ') v++;
            if (strncasecmp(v, "close", 5) == 0)      req->keep_alive = false;
            else if (strncasecmp(v, "keep-alive", 10) == 0) req->keep_alive = true;
        }
    }

    /* body + Content-Length */
    const char *body = hdr_end + 4;
    long clen = 0;
    const char *cl = ci_find(buf, "Content-Length:");
    if (cl) clen = strtol(cl + 15, NULL, 10);
    size_t have_body = total - (size_t)(body - buf);
    while ((long)have_body < clen && total < cap - 1) {
        int n = (int)recv(conn, buf + total, (int)(cap - 1 - total), 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        have_body = total - (size_t)(body - buf);
    }
    req->body = body;
    return true;
}

/* One client connection, handled on its own thread. The real host (Express)
 * uses HTTP keep-alive: the Quest reuses ONE connection for pair -> heartbeat ->
 * start -> device. We honor that — read requests in a loop until the client
 * asks to close, times out, or hangs up. A recv timeout reclaims an idle/dead
 * keep-alive socket (the heartbeat interval is ~well under this). */
struct conn_arg {
    struct bsdr_control *c;
    bsdr_socket_t conn;
    char remote_ip[INET_ADDRSTRLEN];
};

static void conn_thread(void *arg) {
    struct conn_arg *ca = (struct conn_arg *)arg;
    /* idle/read timeout: longer than the 15 s heartbeat window, with margin */
#ifdef _WIN32
    DWORD tv = 60000;   /* ms */
#else
    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
#endif
    setsockopt(ca->conn, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    char *buf = malloc(8192);
    while (buf && ca->c->running) {
        http_req req;
        if (!read_request(ca->conn, buf, 8192, &req)) break;   /* close/timeout/EOF */
        handle_request(ca->c, ca->conn, &req, ca->remote_ip);
        if (!req.keep_alive) break;
    }
    free(buf);
    bsdr_socket_close(ca->conn);
    free(ca);
}

static void control_loop(void *arg) {
    struct bsdr_control *c = (struct bsdr_control *)arg;
    while (c->running) {
        struct sockaddr_in from;
        bsdr_socket_t conn = bsdr_tcp_accept(c->listener, &from);
        if (conn == BSDR_INVALID_SOCKET) {
            if (c->running) bsdr_sleep_ms(20);
            continue;
        }
        struct conn_arg *ca = calloc(1, sizeof(*ca));
        if (!ca) { bsdr_socket_close(conn); continue; }
        ca->c = c;
        ca->conn = conn;
        bsdr_sockaddr_ip(&from, ca->remote_ip, sizeof(ca->remote_ip));
        /* per-connection thread so an idle keep-alive socket never starves the
         * accept loop (the single-threaded one-shot model dropped heartbeats) */
        if (!bsdr_thread_start_detached(conn_thread, ca)) {
            conn_thread(ca);   /* fallback: handle inline if thread spawn fails */
        }
    }
}

bsdr_control *bsdr_control_start(const char *pairing_code,
                                 const bsdr_control_cbs *cbs) {
    struct bsdr_control *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    snprintf(c->pairing_code, sizeof(c->pairing_code), "%s", pairing_code);
    if (cbs) c->cbs = *cbs;
    c->lock = bsdr_mutex_new();
    c->listener = bsdr_tcp_listen("0.0.0.0", BSDR_HTTP_SERVER_PORT, 8);
    if (c->listener == BSDR_INVALID_SOCKET) {
        BSDR_ERROR("bsdr.control", "listen tcp/%d failed: %s",
                   BSDR_HTTP_SERVER_PORT, bsdr_socket_strerror());
        bsdr_mutex_free(c->lock);
        free(c);
        return NULL;
    }
    bsdr_set_nonblocking(c->listener);   /* so the accept loop can observe running=0 */
    c->running = 1;
    c->thread = bsdr_thread_start(control_loop, c);
    BSDR_INFO("bsdr.control", "control server on 0.0.0.0:%d", BSDR_HTTP_SERVER_PORT);
    return c;
}

void bsdr_control_stop(bsdr_control *c) {
    if (!c) return;
    c->running = 0;
    bsdr_socket_close(c->listener);   /* unblock accept */
    if (c->thread) bsdr_thread_join(c->thread);
    bsdr_mutex_free(c->lock);
    free(c);
}

bool bsdr_control_force_unpair(bsdr_control *c) {
    bsdr_mutex_lock(c->lock);
    bool had = c->have_device;
    if (had) {
        BSDR_INFO("bsdr.control", "operator disconnected device %s",
                  c->device.device_name);
        c->have_device = false;
    }
    bsdr_mutex_unlock(c->lock);
    return had;
}

bool bsdr_control_expire_stale(bsdr_control *c) {
    bool expired = false;
    bsdr_mutex_lock(c->lock);
    if (c->have_device) {
        double idle_ms = difftime(time(NULL), c->device.last_keepalive) * 1000.0;
        if (idle_ms > BSDR_FORGET_UNRESPONSIVE_DEVICE_MS) {
            BSDR_INFO("bsdr.control", "forgetting unresponsive device %s",
                      c->device.device_name);
            c->have_device = false;
            expired = true;
        }
    }
    bsdr_mutex_unlock(c->lock);
    return expired;
}
