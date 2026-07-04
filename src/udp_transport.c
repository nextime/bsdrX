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
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE        /* sendmmsg / struct mmsghdr */
#endif
#include "bsdr/udp_transport.h"
#include "bsdr/net.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#endif

bool bsdr_udp_open(bsdr_udp *u, uint16_t local_port,
                   const char *remote_ip, uint16_t remote_port) {
    memset(u, 0, sizeof(*u));
    u->sock = bsdr_udp_bind("0.0.0.0", local_port, false);
    if (u->sock == BSDR_INVALID_SOCKET) {
        BSDR_ERROR("bsdr.udp", "bind udp/%u failed: %s", local_port,
                   bsdr_socket_strerror());
        return false;
    }
    if (remote_ip && *remote_ip) {
        if (!bsdr_sockaddr_make(&u->remote, remote_ip, remote_port)) {
            BSDR_ERROR("bsdr.udp", "bad remote %s", remote_ip);
            return false;
        }
        u->have_remote = true;
    }
    BSDR_INFO("bsdr.udp", "udp bound 0.0.0.0:%u -> %s", local_port,
              u->have_remote ? remote_ip : "(await peer)");
    return true;
}

int bsdr_udp_send(bsdr_udp *u, const void *buf, size_t len) {
    if (!u->have_remote) {
        BSDR_WARN("bsdr.udp", "dropping %zuB: no remote peer yet", len);
        return -1;
    }
    return bsdr_udp_sendto(u->sock, buf, len, &u->remote);
}

/* Send `count` datagrams (each iov[i]) to the remote in as few syscalls as possible (sendmmsg).
 * Falls back to a sendto loop where sendmmsg is unavailable. Returns datagrams sent, -1 on error. */
int bsdr_udp_send_batch(bsdr_udp *u, const struct iovec *iov, int count) {
    if (!u->have_remote || count <= 0) return -1;
#if defined(__linux__)
    /* Linux: batch the burst into one syscall with sendmmsg (glibc extension). */
    struct mmsghdr msgs[64];
    int sent = 0;
    for (int off = 0; off < count; ) {
        int n = count - off; if (n > 64) n = 64;
        for (int i = 0; i < n; i++) {
            memset(&msgs[i], 0, sizeof(msgs[i]));
            msgs[i].msg_hdr.msg_iov    = (struct iovec *)&iov[off + i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name   = &u->remote;
            msgs[i].msg_hdr.msg_namelen = sizeof(u->remote);
        }
        int r = sendmmsg(u->sock, msgs, n, 0);
        if (r < 0) return sent ? sent : -1;
        sent += r; off += r;
        if (r < n) break;   /* partial: kernel buffer full, drop the rest of this burst */
    }
    return sent;
#else
    /* Windows + macOS/BSD: no sendmmsg — one sendto per datagram. */
    int ok = 0;
    for (int i = 0; i < count; i++)
        if (bsdr_udp_sendto(u->sock, iov[i].iov_base, iov[i].iov_len, &u->remote) < 0) ok = -1;
    return ok < 0 ? -1 : count;
#endif
}

int bsdr_udp_recv(bsdr_udp *u, void *buf, size_t len, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(u->sock, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select((int)(u->sock + 1), &rfds, NULL, NULL, &tv);
    if (r == 0) return 0;            /* timeout */
    if (r < 0) return -1;

    struct sockaddr_in from;
    int n = bsdr_udp_recvfrom(u->sock, buf, len, &from);
    if (n < 0) return -1;
    if (!u->have_remote) {           /* responder: latch onto first peer */
        u->remote = from;
        u->have_remote = true;
        char ip[INET_ADDRSTRLEN];
        bsdr_sockaddr_ip(&from, ip, sizeof(ip));
        BSDR_DEBUG("bsdr.udp", "learned remote peer %s", ip);
    }
    if (n > 0) {
        const unsigned char *pb = (const unsigned char *)buf;
        unsigned b0 = pb[0];
        /* Suppress the high-rate floods (DTLS records + SRTP/RTP media) but DO log the
         * rare/interesting packets: SCTP control, RTCP feedback, beacons, pre-DTLS.
         * RTCP (V=2, PT 200-207) rides the 128-191 range but must NOT be suppressed. */
        bool is_rtcp = (b0 >= 128 && b0 < 192) && n >= 2 && pb[1] >= 200 && pb[1] <= 207;
        bool suppress = ((b0 >= 20 && b0 <= 23) || (b0 >= 128 && b0 < 192)) && !is_rtcp;
        if (!suppress) {
            char hex[3 * 32 + 8] = {0};
            int cap = n < 32 ? n : 32, p = 0;
            for (int i = 0; i < cap; i++)
                p += snprintf(hex + p, sizeof(hex) - p, "%02x ",
                              ((const unsigned char *)buf)[i]);
            char sip[INET_ADDRSTRLEN];
            bsdr_sockaddr_ip(&from, sip, sizeof(sip));
            BSDR_DEBUG("bsdr.udp", "rx %dB [%s] from %s:%u  %s%s", n,
                       bsdr_udp_classify(buf, (size_t)n), sip, ntohs(from.sin_port),
                       hex, n > 32 ? "..." : "");
        }
    }
    return n;
}

void bsdr_udp_close(bsdr_udp *u) {
    if (u->sock != BSDR_INVALID_SOCKET) {
        bsdr_socket_close(u->sock);
        u->sock = BSDR_INVALID_SOCKET;
    }
}

uint16_t bsdr_udp_local_port(bsdr_udp *u) {
    if (!u || u->sock == BSDR_INVALID_SOCKET) return 0;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (getsockname(u->sock, (struct sockaddr *)&sa, &sl) != 0) return 0;
    return ntohs(sa.sin_port);
}

const char *bsdr_udp_classify(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    if (len == 0) return "empty";
    unsigned b = p[0];
    if (b == 20) return "DTLS-ChangeCipherSpec";
    if (b == 21) return "DTLS-Alert";
    if (b == 22) return "DTLS-Handshake";
    if (b == 23) return "DTLS-AppData(SCTP)";
    /* SCTP-over-UDP (mediasoup data channel: src=dst=5000). Decode the first chunk type. */
    if (len >= 13 && p[0] == 0x13 && p[1] == 0x88 && p[2] == 0x13 && p[3] == 0x88) {
        switch (p[12]) {
            case 0:  return "SCTP DATA";
            case 1:  return "SCTP INIT";
            case 2:  return "SCTP INIT-ACK";
            case 3:  return "SCTP SACK";
            case 4:  return "SCTP HEARTBEAT";
            case 5:  return "SCTP HEARTBEAT-ACK";
            case 6:  return "SCTP ABORT";
            case 7:  return "SCTP SHUTDOWN";
            case 8:  return "SCTP SHUTDOWN-ACK";
            case 9:  return "SCTP ERROR";
            case 10: return "SCTP COOKIE-ECHO";
            case 11: return "SCTP COOKIE-ACK";
            case 14: return "SCTP SHUTDOWN-COMPLETE";
            default: return "SCTP (other chunk)";
        }
    }
    /* RTP/RTCP (V=2, 0x80-0xBF). RTCP if the payload type is 200-207. */
    if (b >= 128 && b < 192 && len >= 2) {
        unsigned pt = p[1], fmt = b & 0x1f;
        if (pt < 200 || pt > 207) return "RTP media";
        switch (pt) {
            case 200: return "RTCP SR";
            case 201: return "RTCP RR";
            case 202: return "RTCP SDES";
            case 203: return "RTCP BYE";
            case 204: return "RTCP APP";
            case 205: return fmt == 1 ? "RTCP NACK" : "RTCP RTPFB";
            case 206: return fmt == 1 ? "RTCP PLI (keyframe req)"
                           : fmt == 4 ? "RTCP FIR (keyframe req)"
                           : fmt == 15 ? "RTCP REMB" : "RTCP PSFB";
            default:  return "RTCP";
        }
    }
    if (b == 0 || b == 1) return "STUN? (unexpected: no ICE)";
    return "unknown";
}
