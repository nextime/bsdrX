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
#include "bsdr/discovery.h"
#include "bsdr/protocol.h"
#include "bsdr/net.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The discovery reply carries the live pairing code (the Quest reads it from the reply to pair —
 * a protocol requirement we can't drop). To keep it from being an internet reflector / pairing-code
 * oracle, only answer sources on the local link: loopback, RFC1918, and link-local. A real headset
 * is always on the LAN; a spoofed public source now gets nothing. */
static bool addr_is_local(const struct sockaddr_in *a) {
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    return (ip >> 24) == 10 ||                       /* 10.0.0.0/8      */
           (ip >> 20) == 0xAC1 ||                    /* 172.16.0.0/12   */
           (ip >> 16) == 0xC0A8 ||                   /* 192.168.0.0/16  */
           (ip >> 24) == 127 ||                      /* 127.0.0.0/8     */
           (ip >> 16) == 0xA9FE;                     /* 169.254.0.0/16  */
}

struct bsdr_discovery {
    bsdr_discovery_info info;
    bsdr_socket_t sock;
    bsdr_thread *thread;
    volatile int running;
    void (*on_seen)(const char *ip, void *user);
    void *on_seen_user;
};

void bsdr_discovery_set_on_seen(bsdr_discovery *d,
                                void (*cb)(const char *ip, void *user), void *user) {
    d->on_seen = cb;
    d->on_seen_user = user;
}

size_t bsdr_discovery_build(const bsdr_discovery_info *info,
                            uint8_t *out, size_t outlen) {
    if (outlen < BSDR_BROADCAST_HEADER_LEN) return 0;
    memcpy(out, BSDR_BROADCAST_HEADER, BSDR_BROADCAST_HEADER_LEN);
    char *json = (char *)out + BSDR_BROADCAST_HEADER_LEN;
    size_t cap = outlen - BSDR_BROADCAST_HEADER_LEN;
    int n = snprintf(json, cap,
        "{\"sessionId\":\"%s\",\"version\":\"%s\",\"deviceName\":\"%s\","
        "\"deviceId\":\"%s\",\"pairingRequestCode\":\"%s\"}",
        info->session_id, info->version, info->device_name,
        info->device_id, info->pairing_request_code);
    if (n < 0 || (size_t)n >= cap) return 0;
    return BSDR_BROADCAST_HEADER_LEN + (size_t)n;
}

static void discovery_loop(void *arg) {
    struct bsdr_discovery *d = (struct bsdr_discovery *)arg;
    uint8_t in[64];
    uint8_t resp[512];
    while (d->running) {
        struct sockaddr_in from;
        int n = bsdr_udp_recvfrom(d->sock, in, sizeof(in), &from);
        if (n < 0) {
            if (d->running) bsdr_sleep_ms(50);
            continue;
        }
        if (!bsdr_check_message_header(in, (size_t)n)) {
            BSDR_DEBUG("bsdr.discovery", "ignoring %d-byte non-discovery packet", n);
            continue;
        }
        if (!addr_is_local(&from)) {
            BSDR_DEBUG("bsdr.discovery", "ignoring discovery from non-local source");
            continue;   /* don't leak the pairing code / reflect to off-LAN addresses */
        }
        char host[INET_ADDRSTRLEN];
        bsdr_sockaddr_ip(&from, host, sizeof(host));
        /* Reply FIRST, before any app-state callback. The Quest drops the pairing if it doesn't get a
         * discovery reply within 10s of its 5s broadcast (UDP-discovery liveness timer in the client's
         * RemoteDesktopClient — NOT the HTTP heartbeat). on_seen takes the app lock, which cloud HTTPS
         * ops (login/token-renew/rooms) can hold for seconds; doing it before the reply would let that
         * delay starve the liveness timer and cause spurious unpair/re-pair cycles. */
        size_t rlen = bsdr_discovery_build(&d->info, resp, sizeof(resp));
        if (rlen == 0) { BSDR_WARN("bsdr.discovery", "reply build failed (fields too long)"); continue; }
        struct sockaddr_in reply = from;
        reply.sin_port = htons(BSDR_REMOTE_CLIENT_INFO_PORT);
        bsdr_udp_sendto(d->sock, resp, rlen, &reply);
        BSDR_INFO("bsdr.discovery", "discovery request from %s", host);
        if (d->on_seen) d->on_seen(host, d->on_seen_user);
    }
}

bsdr_discovery *bsdr_discovery_start(const bsdr_discovery_info *info) {
    struct bsdr_discovery *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->info = *info;
    d->sock = bsdr_udp_bind("0.0.0.0", BSDR_DISCOVERY_REQUEST_PORT, true);
    if (d->sock == BSDR_INVALID_SOCKET) {
        BSDR_ERROR("bsdr.discovery", "bind udp/%d failed: %s",
                   BSDR_DISCOVERY_REQUEST_PORT, bsdr_socket_strerror());
        free(d);
        return NULL;
    }
    bsdr_set_nonblocking(d->sock);   /* so the recv loop can observe running=0 */
    d->running = 1;
    d->thread = bsdr_thread_start(discovery_loop, d);
    BSDR_INFO("bsdr.discovery", "discovery responder listening on 0.0.0.0:%d",
              BSDR_DISCOVERY_REQUEST_PORT);
    return d;
}

void bsdr_discovery_stop(bsdr_discovery *d) {
    if (!d) return;
    d->running = 0;
    bsdr_socket_close(d->sock);   /* unblock recvfrom */
    if (d->thread) bsdr_thread_join(d->thread);
    free(d);
}
