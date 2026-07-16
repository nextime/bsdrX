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
/* SCTP-over-DTLS WebRTC DataChannel via usrsctp — BSDR_ENABLE_SCTP.
 *
 * Single-threaded: usrsctp_init_nothreads() means usrsctp never calls our DTLS
 * output from its own thread; we drive timers from the pump (handle_timers). So
 * conn_output only fires synchronously from feed/send/handle_timers on the pump
 * thread, and no locking around the (non-thread-safe) DTLS object is needed.
 *
 * DCEP per RFC 8832 (DATA_CHANNEL_OPEN=0x03 -> DATA_CHANNEL_ACK=0x02); binary
 * messages on PPID 53 (RFC 8831).
 */
#include "bsdr/sctp.h"
#include "bsdr/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <usrsctp.h>
#if defined(_WIN32)
#  include "bsdr/platform.h"   /* winsock2: struct linger / SO_LINGER */
#else
#  include <sys/socket.h>      /* struct linger / SO_LINGER */
#endif

#define PPID_DCEP          50
#define PPID_STRING        51
#define PPID_BINARY        53
#define PPID_STRING_EMPTY  56
#define PPID_BINARY_EMPTY  57

#define DCEP_OPEN  0x03
#define DCEP_ACK   0x02


struct bsdr_sctp {
    bsdr_dtls *dtls;
    bsdr_udp  *udp;          /* non-NULL => raw SCTP-over-UDP (cloud), no DTLS */
    struct socket *sock;     /* data socket (connected or accepted) */
    struct socket *lsock;    /* listen socket (responder only) */
    bool initiator;
    uint16_t port;
    bsdr_dc_msg_cb cb;
    void *user;
    bool associated;
    bool failed;        /* relay ABORTed / COMM_LOST — recreate + retry to associate */
    bool channel_open;
    uint16_t stream;         /* the DataChannel stream id */
    char pending_label[64];  /* channel to open once associated, or "" */
    bool want_open;
};

static int g_users = 0;

/* usrsctp -> wire: wrap the SCTP packet in DTLS application data. */
static int conn_output(void *addr, void *buf, size_t len, uint8_t tos, uint8_t set_df) {
    (void)tos; (void)set_df;
    struct bsdr_sctp *s = (struct bsdr_sctp *)addr;
    unsigned char *p = (unsigned char *)buf;
    unsigned chunk = len > 12 ? p[12] : 255;
    if (s->udp) {
        /* Cloud: raw SCTP-over-UDP. Send exactly what usrsctp produced — which now carries a
         * REAL CRC32c (offload disabled), byte-identical to the official host's data-port INIT
         * (verified: src/dst SCTP port 5000, OS/MIS 256, valid CRC32c). The relay never replies
         * (it is a receive-only comedia sink — even the official host gets ZERO packets back), so
         * "associated" never flips; that is EXPECTED, not a failure. */
        /* SACK(3)/HEARTBEAT(4)/HEARTBEAT-ACK(5) repeat forever while the association is held open
         * (they dominated debug.log 10:1). Log the first of each, then 1-in-512; anything else
         * (INIT/DATA/ABORT/COOKIE/SHUTDOWN) always logs — those are the events worth seeing. */
        static unsigned long ka = 0;
        bool keepalive = (chunk == 3 || chunk == 4 || chunk == 5);
        if (!keepalive || (ka++ % 512) == 0)
            BSDR_DEBUG("bsdr.sctp", "tx %zu B (chunk=%u assoc=%d)%s -> relay UDP", len, chunk,
                       s->associated, keepalive ? " [keepalive, 1/512]" : "");
        int n = bsdr_udp_send(s->udp, buf, len);
        return n < 0 ? EFAULT : 0;
    }
    /* LAN: SCTP-over-DTLS — force the CRC32c to 0 (DTLS provides integrity; the
     * headset's RFC-8261 SCTP drops a non-zero checksum). */
    if (len >= 12) memset(p + 8, 0, 4);
    BSDR_DEBUG("bsdr.sctp", "tx %zu B (chunk=%u assoc=%d, csum=0)", len, chunk, s->associated);
    int n = bsdr_dtls_send(s->dtls, buf, len);
    return n < 0 ? EFAULT : 0;
}

static void set_common_opts(struct socket *so) {
    int on = 1;
    usrsctp_setsockopt(so, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on));
    usrsctp_setsockopt(so, IPPROTO_SCTP, SCTP_EXPLICIT_EOR, &on, sizeof(on));
    struct sctp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.se_assoc_id = SCTP_FUTURE_ASSOC;
    ev.se_on = 1;
    ev.se_type = SCTP_ASSOC_CHANGE;
    usrsctp_setsockopt(so, IPPROTO_SCTP, SCTP_EVENT, &ev, sizeof(ev));
    struct sctp_initmsg im;
    memset(&im, 0, sizeof(im));
    im.sinit_num_ostreams = 256;   /* match BigSoup (SCTP_INITMSG 256/256) */
    im.sinit_max_instreams = 256;
    usrsctp_setsockopt(so, IPPROTO_SCTP, SCTP_INITMSG, &im, sizeof(im));
    usrsctp_set_non_blocking(so, 1);
}

static void send_dcep_ack(struct bsdr_sctp *s, uint16_t stream) {
    uint8_t ack = DCEP_ACK;
    struct sctp_sndinfo si;
    memset(&si, 0, sizeof(si));
    si.snd_sid = stream;
    si.snd_ppid = htonl(PPID_DCEP);
    si.snd_flags = SCTP_EOR;
    usrsctp_sendv(s->sock, &ack, 1, NULL, 0, &si, sizeof(si),
                  SCTP_SENDV_SNDINFO, 0);
}

static void handle_dcep(struct bsdr_sctp *s, const uint8_t *data, size_t len,
                        uint16_t stream) {
    if (len < 1) return;
    if (data[0] == DCEP_OPEN) {
        s->stream = stream;
        s->channel_open = true;
        send_dcep_ack(s, stream);
        BSDR_INFO("bsdr.sctp", "DataChannel opened by peer (stream %u) -> ACK", stream);
    } else if (data[0] == DCEP_ACK) {
        s->channel_open = true;
        BSDR_INFO("bsdr.sctp", "DataChannel ACK (stream %u)", s->stream);
    }
}

static void handle_notification(struct bsdr_sctp *s, const uint8_t *buf, size_t len) {
    if (len < sizeof(struct sctp_assoc_change)) return;
    const union sctp_notification *n = (const union sctp_notification *)buf;
    if (n->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
        const struct sctp_assoc_change *ac = &n->sn_assoc_change;
        /* states: 1=COMM_UP 2=COMM_LOST 3=RESTART 4=SHUTDOWN_COMP 5=CANT_STR_ASSOC */
        BSDR_INFO("bsdr.sctp", "assoc_change state=%u (in=%u out=%u)",
                  ac->sac_state, ac->sac_inbound_streams, ac->sac_outbound_streams);
        if (ac->sac_state == SCTP_COMM_UP) {
            s->associated = true;
            BSDR_INFO("bsdr.sctp", "SCTP association up");
            if (s->want_open) {
                s->want_open = false;
                bsdr_sctp_open_channel(s, s->pending_label);
            }
        } else if (ac->sac_state == SCTP_COMM_LOST || ac->sac_state == SCTP_CANT_STR_ASSOC) {
            /* relay ABORTed our INIT (its stale association from a prior share collided). Its
             * ABORT clears that stale assoc, so the caller can recreate + retry and succeed. */
            s->failed = true;
        }
    } else {
        BSDR_DEBUG("bsdr.sctp", "notification type=%u", n->sn_header.sn_type);
    }
}

static void drain(struct bsdr_sctp *s, struct socket *so) {
    for (;;) {
        uint8_t buf[4096];
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);
        struct sctp_rcvinfo rcv;
        socklen_t infolen = sizeof(rcv);
        unsigned int infotype = 0;
        int flags = 0;
        ssize_t n = usrsctp_recvv(so, buf, sizeof(buf),
                                  (struct sockaddr *)&from, &fromlen,
                                  &rcv, &infolen, &infotype, &flags);
        if (n <= 0) break;
        if (flags & MSG_NOTIFICATION) {
            handle_notification(s, buf, (size_t)n);
            continue;
        }
        uint32_t ppid = ntohl(rcv.rcv_ppid);
        uint16_t stream = rcv.rcv_sid;
        /* Show what the relay actually sends — ppid/stream + an ASCII preview so
         * we can tell room-data/JSON (string) from input opcodes (binary). */
        char prev[81]; int pl = 0, pc = n < 64 ? (int)n : 64;
        for (int i = 0; i < pc; i++) {
            unsigned c = buf[i];
            prev[pl++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        prev[pl] = 0;
        BSDR_DEBUG("bsdr.sctp", "rx msg ppid=%u stream=%u len=%zd: %s%s",
                   ppid, stream, n, prev, n > 64 ? "..." : "");
        if (ppid == PPID_DCEP) {
            handle_dcep(s, buf, (size_t)n, stream);
        } else if (ppid == PPID_BINARY || ppid == PPID_BINARY_EMPTY) {
            if (s->cb) s->cb(buf, (size_t)n, s->user);
        } else if (ppid == PPID_STRING || ppid == PPID_STRING_EMPTY) {
            /* Mediasoup room/producer signaling arrives as JSON strings — deliver
             * it too so the cloud layer can react (the input decoder ignores
             * non-opcode payloads harmlessly). */
            if (s->cb) s->cb(buf, (size_t)n, s->user);
        }
    }
}

static void upcall(struct socket *so, void *arg, int flags) {
    (void)flags;
    struct bsdr_sctp *s = (struct bsdr_sctp *)arg;
    if (s->lsock && so == s->lsock && s->sock == NULL) {
        struct sockaddr_storage ra;
        socklen_t ral = sizeof(ra);
        struct socket *c = usrsctp_accept(s->lsock, (struct sockaddr *)&ra, &ral);
        if (c) {
            s->sock = c;
            s->associated = true;
            set_common_opts(c);
            usrsctp_set_upcall(c, upcall, s);
            BSDR_INFO("bsdr.sctp", "SCTP association accepted");
            if (s->want_open) {
                s->want_open = false;
                bsdr_sctp_open_channel(s, s->pending_label);
            }
            drain(s, c);   /* data may already be queued */
        }
        return;
    }
    drain(s, so);
}

static bsdr_sctp *sctp_new_common(bsdr_dtls *dtls, bsdr_udp *udp, bool initiator,
                                  bsdr_dc_msg_cb cb, void *user) {
    struct bsdr_sctp *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dtls = dtls;
    s->udp = udp;
    s->initiator = initiator;
    s->cb = cb;
    s->user = user;
    s->pending_label[0] = '\0';
    if (g_users++ == 0) {
        usrsctp_init_nothreads(0, conn_output, NULL);
        /* Let usrsctp compute the REAL SCTP CRC32c. The CLOUD relay requires it — a live
         * capture of the OFFICIAL host's data-port INIT shows a valid CRC32c (e.g. 7e25ec6a),
         * NOT csum=0 (an earlier reading of BigSoup's conn_output was wrong). The LAN/DTLS path
         * still needs csum=0 (RFC 8261) — conn_output zeroes it explicitly there. So do NOT
         * enable crc32c offload (which would leave every checksum 0). */
    }
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    /* Fast INIT retransmit: the headset's SCTP listener may come up a beat after
     * the DTLS handshake and it crashes ~1s in; default RTO (~3s) misses that
     * window, so retry the INIT every ~200ms (backing off to ~800ms). */
    usrsctp_sysctl_set_sctp_rto_initial_default(200);
    usrsctp_sysctl_set_sctp_rto_min_default(100);
    usrsctp_sysctl_set_sctp_rto_max_default(800);
    usrsctp_sysctl_set_sctp_init_rto_max_default(800);
    usrsctp_register_address(s);
    return s;
}

bsdr_sctp *bsdr_sctp_new(bsdr_dtls *dtls, bool initiator,
                         bsdr_dc_msg_cb cb, void *user) {
    return sctp_new_common(dtls, NULL, initiator, cb, user);
}

bsdr_sctp *bsdr_sctp_new_udp(bsdr_udp *udp, bool initiator,
                             bsdr_dc_msg_cb cb, void *user) {
    return sctp_new_common(NULL, udp, initiator, cb, user);
}

bool bsdr_sctp_start(bsdr_sctp *s, uint16_t port) {
    s->port = port;
    struct socket *so = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP,
                                       NULL, NULL, 0, NULL);
    if (!so) { BSDR_ERROR("bsdr.sctp", "usrsctp_socket failed"); return false; }
    set_common_opts(so);

    struct sockaddr_conn sconn;
    memset(&sconn, 0, sizeof(sconn));
    sconn.sconn_family = AF_CONN;
    sconn.sconn_port = htons(port);
    sconn.sconn_addr = s;
    if (usrsctp_bind(so, (struct sockaddr *)&sconn, sizeof(sconn)) < 0) {
        BSDR_ERROR("bsdr.sctp", "usrsctp_bind failed: %s", strerror(errno));
        usrsctp_close(so);
        return false;
    }

    if (s->initiator) {
        usrsctp_set_upcall(so, upcall, s);
        s->sock = so;
        struct sockaddr_conn raddr;
        memset(&raddr, 0, sizeof(raddr));
        raddr.sconn_family = AF_CONN;
        raddr.sconn_port = htons(port);
        raddr.sconn_addr = s;
        int cr = usrsctp_connect(so, (struct sockaddr *)&raddr, sizeof(raddr));
        BSDR_INFO("bsdr.sctp", "usrsctp_connect (active) port %u -> %s", port,
                  (cr == 0 || errno == EINPROGRESS) ? "in progress" : strerror(errno));
        if (cr < 0 && errno != EINPROGRESS) {
            BSDR_ERROR("bsdr.sctp", "usrsctp_connect failed: %s", strerror(errno));
            return false;
        }
    } else {
        if (usrsctp_listen(so, 1) < 0) {
            BSDR_ERROR("bsdr.sctp", "usrsctp_listen failed");
            usrsctp_close(so);
            return false;
        }
        s->lsock = so;
        usrsctp_set_upcall(so, upcall, s);
    }
    return true;
}

void bsdr_sctp_feed(bsdr_sctp *s, const uint8_t *data, size_t len) {
    unsigned chunk = len > 12 ? data[12] : 255;
    BSDR_DEBUG("bsdr.sctp", "rx %zu B (chunk=%u)", len, chunk);
    usrsctp_conninput(s, data, len, 0);
}

void bsdr_sctp_handle_timers(uint32_t elapsed_ms) {
    usrsctp_handle_timers(elapsed_ms);
}

bool bsdr_sctp_open_channel(bsdr_sctp *s, const char *label) {
    if (!s->sock || !s->associated) {  /* defer until accepted/connected + up */
        snprintf(s->pending_label, sizeof(s->pending_label), "%s", label ? label : "");
        s->want_open = true;
        return true;
    }
    size_t llen = label ? strlen(label) : 0;
    if (llen > 32) llen = 32;
    uint8_t msg[12 + 32];
    memset(msg, 0, sizeof(msg));
    msg[0] = DCEP_OPEN;        /* message type */
    msg[1] = 0x00;            /* channel type: reliable, ordered */
    /* priority (2) + reliability (4) = 0; label_length (8..9), protocol_length (10..11) */
    msg[8] = (uint8_t)(llen >> 8);
    msg[9] = (uint8_t)(llen & 0xff);
    if (llen) memcpy(msg + 12, label, llen);

    s->stream = 0;            /* opener uses stream 0 */
    struct sctp_sndinfo si;
    memset(&si, 0, sizeof(si));
    si.snd_sid = s->stream;
    si.snd_ppid = htonl(PPID_DCEP);
    si.snd_flags = SCTP_EOR;
    ssize_t n = usrsctp_sendv(s->sock, msg, 12 + llen, NULL, 0, &si, sizeof(si),
                              SCTP_SENDV_SNDINFO, 0);
    if (n < 0) { BSDR_ERROR("bsdr.sctp", "DCEP open send failed: %s", strerror(errno)); return false; }
    BSDR_INFO("bsdr.sctp", "opened DataChannel '%s' on stream %u", label ? label : "", s->stream);
    return true;
}

bool bsdr_sctp_channel_open(bsdr_sctp *s) { return s->channel_open; }
bool bsdr_sctp_associated(bsdr_sctp *s) { return s->associated; }
bool bsdr_sctp_failed(bsdr_sctp *s) { return s && s->failed; }

int bsdr_sctp_send(bsdr_sctp *s, const uint8_t *data, size_t len) {
    if (!s->sock) return -1;
    struct sctp_sndinfo si;
    memset(&si, 0, sizeof(si));
    si.snd_sid = s->stream;
    si.snd_ppid = htonl(PPID_BINARY);
    si.snd_flags = SCTP_EOR;
    return (int)usrsctp_sendv(s->sock, data, len, NULL, 0, &si, sizeof(si),
                              SCTP_SENDV_SNDINFO, 0);
}

int bsdr_sctp_send_room(bsdr_sctp *s, const uint8_t *data, size_t len) {
    if (!s || !s->sock) return -1;
    /* BigSoup sends on stream 1, no DCEP channel setup (the relay accepts stream-1 data blindly and
     * fans it out). The payload is an ASCII string ("<legacyId>*base64(code+body)"), so the channel is
     * WebRTC-string — the native lib emits the PPID BYTE-SWAPPED: on-wire ppid is exactly 33 00 00 00
     * (proven across all 389 data messages in room.pcap). Reproduce that verbatim, else the Quest's
     * native BigSoup drops the frame before OnData().
     *
     * UNRELIABLE + UNORDERED (PR-SCTP, 0 retransmits): this is the avatar/pose channel and the relay is
     * a receive-only comedia sink that NEVER SACKs our DATA (even the official host gets zero packets
     * back). Sending reliably there makes usrsctp retransmit forever — the unacked queue grows without
     * bound and the relay fans every retransmitted copy to the headset, flooding its data consumer until
     * it CRASHES (observed: tx bundles growing 276→540→804→1068 B). Fire-and-forget matches the schema
     * (TickState is the unreliable channel) and how a WebRTC unreliable/unordered DataChannel behaves:
     * each frame is transmitted once and abandoned if not delivered, so the queue can never pile up. */
    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID | SCTP_SEND_PRINFO_VALID;
    spa.sendv_sndinfo.snd_sid   = 1;
    spa.sendv_sndinfo.snd_ppid  = htonl(0x33000000u);          /* wire bytes 33 00 00 00 */
    spa.sendv_sndinfo.snd_flags = SCTP_EOR | SCTP_UNORDERED;   /* single message, no head-of-line stall */
    spa.sendv_prinfo.pr_policy  = SCTP_PR_SCTP_RTX;            /* limit by retransmit count... */
    spa.sendv_prinfo.pr_value   = 0;                           /* ...to 0 -> send once, never retransmit */
    return (int)usrsctp_sendv(s->sock, data, len, NULL, 0, &spa, sizeof(spa),
                              SCTP_SENDV_SPA, 0);
}

void bsdr_sctp_free(bsdr_sctp *s) {
    if (!s) return;
    /* ABORT the association on close (SO_LINGER l_onoff=1, l_linger=0) instead of a graceful
     * SHUTDOWN. The comedia relay never answers a SHUTDOWN, so a graceful close leaves the
     * association lingering on the relay — and the next enable's fresh INIT is then ignored
     * (assoc stays 0). An ABORT makes the relay drop it immediately so re-enable can associate. */
    if (s->sock) {
        struct linger lo = { 1, 0 };
        usrsctp_setsockopt(s->sock, SOL_SOCKET, SO_LINGER, &lo, sizeof(lo));
        usrsctp_close(s->sock);
    }
    if (s->lsock && s->lsock != s->sock) usrsctp_close(s->lsock);
    usrsctp_deregister_address(s);
    if (--g_users == 0) {
        for (int i = 0; i < 300 && usrsctp_finish() != 0; i++)
            bsdr_sleep_ms(10);
    }
    free(s);
}
