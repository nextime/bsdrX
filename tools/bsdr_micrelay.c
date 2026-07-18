/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* bsdr_micrelay — router-side owner-mic relay for bsdrX (auto-discovering, multi-headset).
 *
 * Why it exists: a Bigscreen headset sends its microphone only to the Bigscreen cloud (plain Opus
 * RTP); there is no LAN mic upload. On a WIRED LAN bsdrX can ARP-spoof to see that stream, but over
 * WiFi it can't (the AP bridges per-station traffic). The router IS in the path for every packet the
 * headset sends, so this tiny relay captures the headset's outbound UDP with libpcap and forwards the
 * raw IPv4 packets to bsdrX, which decodes them via its normal owner-mic path. Because it captures the
 * headset's OWN uplink, the result is the owner's voice only — no room mixing, no MITM, no monitor mode.
 *
 * This daemon serves MANY headsets/agents at once, zero-config:
 *   - Discovery: it broadcasts a HELLO beacon on BSDR_RELAY_PORT; each bsdrX agent hears it and
 *     REGISTERs (heartbeat) for the headset it is paired with. No IPs/ports are hand-typed.
 *   - Bind-to-owner auth: the relay is in-path, so it watches each headset<->agent bsdrX session
 *     (UDP on the pairing ports) and only forwards headset H's mic to the agent it actually saw
 *     paired with H. A LAN host can't ask the router for a headset it isn't running the session for.
 *   - Parallel: one flow per headset, demuxed by source IP from a single all-UDP capture.
 *   - Voice substitution (per flow): bsdrX ships a modified datagram; the relay iptables-DROPs the
 *     Quest->cloud original and raw-injects bsdrX's version (src=Quest) so the cloud SFU's comedia
 *     latch keeps accepting it. The relay never decodes/encodes — all codec + FX work is in bsdrX.
 *
 * Build (host):   make micrelay
 * Build (router): cross-compile with the router toolchain + libpcap, e.g.
 *   mipsel-openwrt-linux-gcc -O2 -Iinclude -DBSDR_HAVE_PCAP=1 \
 *       tools/bsdr_micrelay.c src/micsniff_capture.c -lpcap -o bsdr_micrelay
 *
 * Usage:  bsdr_micrelay --iface <if> [--port N] [--verbose]
 *         bsdr_micrelay --iface <if> --quest <ip> --to <host:port>   (static single flow, no discovery)
 */
#include "micsniff_capture.h"
#include "bsdr/relayproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }
static int g_verbose = 0;

static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* Is a network-order IPv4 address in a private / non-routable range? (mic to the cloud is public.) */
static int is_private_v4(uint32_t be) {
    uint32_t h = ntohl(be);
    uint8_t a = (h >> 24) & 0xff, b = (h >> 16) & 0xff;
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 1;
    if (a == 192 && b == 168) return 1;
    if (a == 169 && b == 254) return 1;
    if (a == 100 && b >= 64 && b <= 127) return 1;
    if (a == 127 || a >= 224 || a == 0) return 1;
    return 0;
}

static const char *ip_str(uint32_t be) {   /* rotating static buffers so two calls in one printf work */
    static char b[4][32]; static int i;
    struct in_addr ia; ia.s_addr = be; i = (i + 1) & 3;
    snprintf(b[i], sizeof b[i], "%s", inet_ntoa(ia));
    return b[i];
}

/* ---------------------------------------------------------------- flow table */
#define MAX_FLOWS   32
#define FLOW_TTL_MS  4000     /* drop a flow this long after its last REGISTER heartbeat */
#define SUB_TTL_MS   2000     /* revert a flow to passthrough this long after its last 'C' control */

typedef struct {
    int      used;
    uint32_t quest_be;               /* headset IP (network order) */
    struct sockaddr_in agent;        /* forward mic + ACK here (the REGISTER source address) */
    uint64_t last_reg_ms;
    int      is_static;              /* from --quest/--to: no discovery, no pairing check, no expiry */
    unsigned long fwd;               /* mic packets forwarded on this flow */
    /* voice substitution */
    int      sub_on, drop_installed;
    uint32_t cloud_be; int cloud_port;
    uint64_t last_ctrl_ms;
    int      cloud_fd;               /* legacy 'A' path */
    int      raw_fd;                 /* IP_HDRINCL raw socket for 'F' full-datagram inject */
} flow_t;

static flow_t g_flows[MAX_FLOWS];

static flow_t *flow_find(uint32_t quest_be) {
    for (int i = 0; i < MAX_FLOWS; i++)
        if (g_flows[i].used && g_flows[i].quest_be == quest_be) return &g_flows[i];
    return NULL;
}
static flow_t *flow_alloc(void) {
    for (int i = 0; i < MAX_FLOWS; i++) if (!g_flows[i].used) { memset(&g_flows[i], 0, sizeof g_flows[i]); g_flows[i].cloud_fd = -1; g_flows[i].raw_fd = -1; return &g_flows[i]; }
    return NULL;
}

/* ---- per-flow iptables DROP of the Quest->cloud transit (so only bsdrX's version reaches cloud) */
static void flow_drop_del(flow_t *f) {
    if (!f->drop_installed) return;
    char c[256];
    snprintf(c, sizeof c, "iptables -D FORWARD -s %s -d %s -p udp --dport %d -j DROP 2>/dev/null",
             ip_str(f->quest_be), ip_str(f->cloud_be), f->cloud_port);
    if (system(c) != 0) { /* best effort */ }
    f->drop_installed = 0;
}
static void flow_drop_add(flow_t *f, uint32_t cloud_be, int port) {
    if (f->drop_installed) {
        if (f->cloud_be == cloud_be && f->cloud_port == port) return;   /* unchanged */
        flow_drop_del(f);
    }
    f->cloud_be = cloud_be; f->cloud_port = port;
    char c[256];
    snprintf(c, sizeof c, "iptables -I FORWARD -s %s -d %s -p udp --dport %d -j DROP 2>/dev/null",
             ip_str(f->quest_be), ip_str(cloud_be), port);
    if (system(c) == 0) { f->drop_installed = 1;
        fprintf(stderr, "bsdr_micrelay: [%s] substitute ON (drop -> %s:%d)\n", ip_str(f->quest_be), ip_str(cloud_be), port);
    } else fprintf(stderr, "bsdr_micrelay: WARNING iptables DROP failed (need root; is iptables present?)\n");
}
static void flow_raw_open(flow_t *f) {
    if (f->raw_fd >= 0) return;
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);   /* IPPROTO_RAW => IP_HDRINCL on */
    if (fd < 0) { fprintf(stderr, "bsdr_micrelay: raw socket failed (need root): %s\n", strerror(errno)); return; }
    int one = 1; (void)setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof one);
    f->raw_fd = fd;
}
static void flow_cloud_reopen(flow_t *f, uint32_t cloud_be, int port) {
    if (f->cloud_fd >= 0) { close(f->cloud_fd); f->cloud_fd = -1; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = cloud_be; a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return; }   /* stable src port */
    f->cloud_fd = fd;
}
static void flow_sub_off(flow_t *f) {
    if (!f->sub_on) return;
    flow_drop_del(f); f->sub_on = 0;
    fprintf(stderr, "bsdr_micrelay: [%s] substitute OFF\n", ip_str(f->quest_be));
}
static void flow_free(flow_t *f) {
    flow_sub_off(f);
    if (f->cloud_fd >= 0) close(f->cloud_fd);
    if (f->raw_fd >= 0) close(f->raw_fd);
    memset(f, 0, sizeof *f);
}

/* ------------------------------------------------------- bind-to-owner pairing observation map */
#define MAX_PAIRS   64
#define PAIR_TTL_MS 20000    /* a headset<->agent binding is trusted for this long after last seen */
typedef struct { uint32_t a, b; uint64_t ts; } pairobs_t;   /* a<b normalised */
static pairobs_t g_pairs[MAX_PAIRS];

static void pair_note(uint32_t x, uint32_t y) {
    uint32_t a = x, b = y;
    if (ntohl(a) > ntohl(b)) { uint32_t t = a; a = b; b = t; }
    uint64_t now = now_ms();
    int oldest = 0; uint64_t oldts = ~0ull;
    for (int i = 0; i < MAX_PAIRS; i++) {
        if (g_pairs[i].a == a && g_pairs[i].b == b) { g_pairs[i].ts = now; return; }
        if (g_pairs[i].ts < oldts) { oldts = g_pairs[i].ts; oldest = i; }
    }
    g_pairs[oldest].a = a; g_pairs[oldest].b = b; g_pairs[oldest].ts = now;   /* evict LRU */
}
static int pair_seen(uint32_t x, uint32_t y) {
    uint32_t a = x, b = y;
    if (ntohl(a) > ntohl(b)) { uint32_t t = a; a = b; b = t; }
    uint64_t now = now_ms();
    for (int i = 0; i < MAX_PAIRS; i++)
        if (g_pairs[i].a == a && g_pairs[i].b == b && now - g_pairs[i].ts < PAIR_TTL_MS) return 1;
    return 0;
}

/* ---------------------------------------------------------------- control socket handlers */
static void send_ack(int fd, const struct sockaddr_in *to, uint32_t quest_be, int status) {
    unsigned char m[16]; int n = bsdr_relay_hdr(m, BSDR_RELAY_ACK);
    memcpy(m + n, &quest_be, 4); n += 4; m[n++] = (unsigned char)status;
    sendto(fd, m, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void handle_ctrl(int fd, const unsigned char *m, int r, const struct sockaddr_in *from) {
    if (!bsdr_relay_is_ctrl(m, r)) return;
    if (m[4] != BSDR_RELAY_VERSION) return;   /* version gate */
    char type = (char)m[5];
    const unsigned char *body = m + 6; int blen = r - 6;

    if (type == BSDR_RELAY_REGISTER && blen >= 11) {
        uint32_t quest_be; memcpy(&quest_be, body, 4);
        int mode = body[4];
        uint32_t cloud_be; memcpy(&cloud_be, body + 5, 4);
        int cport = (body[9] << 8) | body[10];
        /* Bind-to-owner: only serve a headset to the agent we observed it paired with. */
        if (!pair_seen(quest_be, from->sin_addr.s_addr)) {
            if (g_verbose) fprintf(stderr, "bsdr_micrelay: DENY register %s from %s (no observed pairing)\n",
                                   ip_str(quest_be), ip_str(from->sin_addr.s_addr));
            send_ack(fd, from, quest_be, BSDR_RELAY_DENY_UNPAIRED);
            return;
        }
        flow_t *f = flow_find(quest_be);
        int fresh = 0;
        if (!f) { f = flow_alloc(); fresh = 1; }
        if (!f) { fprintf(stderr, "bsdr_micrelay: flow table full, dropping %s\n", ip_str(quest_be)); return; }
        f->used = 1; f->quest_be = quest_be; f->agent = *from; f->last_reg_ms = now_ms();
        (void)cloud_be; (void)cport;   /* cloud dst is authoritative from the 'C' control, not REGISTER */
        if (fresh) fprintf(stderr, "bsdr_micrelay: [%s] registered -> %s:%d%s\n", ip_str(quest_be),
                           ip_str(from->sin_addr.s_addr), ntohs(from->sin_port), mode ? " (substitute)" : "");
        send_ack(fd, from, quest_be, BSDR_RELAY_OK);
    } else if (type == BSDR_RELAY_UNREG && blen >= 4) {
        uint32_t quest_be; memcpy(&quest_be, body, 4);
        flow_t *f = flow_find(quest_be);
        if (f && f->agent.sin_addr.s_addr == from->sin_addr.s_addr) {
            fprintf(stderr, "bsdr_micrelay: [%s] unregistered\n", ip_str(quest_be));
            flow_free(f);
        }
    } else if (type == BSDR_RELAY_CTRL && blen >= 11) {
        uint32_t quest_be; memcpy(&quest_be, body, 4);
        int mode = body[4];
        uint32_t cloud_be; memcpy(&cloud_be, body + 5, 4);
        int cport = (body[9] << 8) | body[10];
        flow_t *f = flow_find(quest_be);
        if (!f || f->agent.sin_addr.s_addr != from->sin_addr.s_addr) return;   /* only the owning agent */
        f->last_ctrl_ms = now_ms();
        if (mode == 1) {
            if (!f->sub_on || f->cloud_fd < 0 || f->cloud_be != cloud_be || f->cloud_port != cport)
                flow_cloud_reopen(f, cloud_be, cport);
            flow_raw_open(f);
            flow_drop_add(f, cloud_be, cport);
            f->sub_on = 1;
        } else flow_sub_off(f);
    } else if (type == BSDR_RELAY_FULLDG && blen > 4 + 20) {
        uint32_t quest_be; memcpy(&quest_be, body, 4);
        flow_t *f = flow_find(quest_be);
        if (!f || !f->sub_on || f->raw_fd < 0 || f->agent.sin_addr.s_addr != from->sin_addr.s_addr) return;
        const unsigned char *dg = body + 4; int dglen = blen - 4;
        struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
        dst.sin_family = AF_INET; memcpy(&dst.sin_addr.s_addr, dg + 16, 4);   /* dst IP from the header */
        (void)sendto(f->raw_fd, dg, (size_t)dglen, 0, (struct sockaddr *)&dst, sizeof dst);
    } else if (type == BSDR_RELAY_RTP && blen > 4) {
        uint32_t quest_be; memcpy(&quest_be, body, 4);
        flow_t *f = flow_find(quest_be);
        if (!f || !f->sub_on || f->cloud_fd < 0 || f->agent.sin_addr.s_addr != from->sin_addr.s_addr) return;
        (void)send(f->cloud_fd, body + 4, (size_t)(blen - 4), 0);
    }
    /* HELLO from agents is informational — the REGISTER carries everything we need. */
}

/* Broadcast a HELLO beacon so agents on the LAN discover us with no configuration. */
static void send_hello(int fd, int port) {
    unsigned char m[8]; int n = bsdr_relay_hdr(m, BSDR_RELAY_HELLO); m[n++] = BSDR_RELAY_ROLE_RELAY;
    struct sockaddr_in b; memset(&b, 0, sizeof b);
    b.sin_family = AF_INET; b.sin_port = htons((uint16_t)port); b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    (void)sendto(fd, m, (size_t)n, 0, (struct sockaddr *)&b, sizeof b);
}

static void usage(const char *p) {
    fprintf(stderr,
        "bsdr_micrelay — forward Bigscreen headset mic RTP to bsdrX from the router (auto, multi-headset).\n\n"
        "  %s --iface <if> [--port N] [--verbose]\n"
        "  %s --iface <if> --quest <ip> --to <host:port>   (static single flow, no discovery/auth)\n\n"
        "  --iface    capture interface the headsets are on (e.g. br-lan)\n"
        "  --port     control/discovery UDP port (default %d)\n"
        "  --verbose  log denied registrations and per-flow detail\n"
        "  --quest    (static) a headset IPv4; with --to, forward its mic there without discovery\n"
        "  --to       (static) bsdrX host:port to forward to\n\n"
        "Auto mode needs no other flags: it broadcasts a beacon, bsdrX agents register for the headset\n"
        "they are paired with, and the relay forwards each headset's mic to its own agent in parallel.\n",
        p, p, BSDR_RELAY_PORT);
}

int main(int argc, char **argv) {
    const char *iface = NULL, *quest = NULL, *to = NULL;
    int port = BSDR_RELAY_PORT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc) iface = argv[++i];
        else if (!strcmp(argv[i], "--quest") && i + 1 < argc) quest = argv[++i];
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    }
    if (!iface) { usage(argv[0]); return 2; }
    if ((quest && !to) || (!quest && to)) { fprintf(stderr, "bsdr_micrelay: --quest and --to go together\n"); return 2; }
    if (port <= 0 || port > 65535) { fprintf(stderr, "bsdr_micrelay: bad --port\n"); return 2; }

    for (int i = 0; i < MAX_FLOWS; i++) { g_flows[i].cloud_fd = -1; g_flows[i].raw_fd = -1; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* Control/discovery socket. */
    int cfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (cfd < 0) { perror("socket"); return 1; }
    int one = 1;
    (void)setsockopt(cfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    (void)setsockopt(cfd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in la; memset(&la, 0, sizeof la);
    la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY); la.sin_port = htons((uint16_t)port);
    if (bind(cfd, (struct sockaddr *)&la, sizeof la) != 0) { perror("bind"); close(cfd); return 1; }

    /* Optional static single flow (bypasses discovery + bind-to-owner: operator asserted it). */
    if (quest && to) {
        char host[128]; int tport = 0;
        const char *colon = strrchr(to, ':');
        if (!colon || colon == to) { fprintf(stderr, "bsdr_micrelay: --to must be host:port\n"); close(cfd); return 2; }
        size_t hn = (size_t)(colon - to); if (hn >= sizeof host) hn = sizeof host - 1;
        memcpy(host, to, hn); host[hn] = 0; tport = atoi(colon + 1);
        flow_t *f = flow_alloc();
        f->used = 1; f->is_static = 1; f->quest_be = inet_addr(quest);
        f->agent.sin_family = AF_INET; f->agent.sin_port = htons((uint16_t)tport);
        if (f->quest_be == INADDR_NONE || inet_pton(AF_INET, host, &f->agent.sin_addr) != 1 || tport <= 0) {
            fprintf(stderr, "bsdr_micrelay: bad --quest/--to\n"); close(cfd); return 2;
        }
        fprintf(stderr, "bsdr_micrelay: static flow [%s] -> %s:%d\n", ip_str(f->quest_be), host, tport);
    }

    char err[256] = "";
    mc_cap *cap = mc_cap_open(iface, NULL, err, sizeof err);   /* NULL = all UDP; we demux + observe */
    if (!cap) { fprintf(stderr, "bsdr_micrelay: capture on %s failed: %s\n(need root / CAP_NET_RAW)\n", iface, err); close(cfd); return 1; }
    fprintf(stderr, "bsdr_micrelay: capturing %s, control udp/%d — auto-discovery %s\n",
            iface, port, (quest && to) ? "off (static)" : "on");

    unsigned char buf[2048];
    uint64_t last_hello = 0;
    const uint16_t pair_ports[] = BSDR_RELAY_PAIR_PORTS;
    unsigned long total_fwd = 0;

    while (!g_stop) {
        /* 1) captured packet: pairing observation + per-flow mic forward. */
        int n = mc_cap_next(cap, buf, sizeof buf, 20);
        if (n < 0) break;
        if (n >= 28 && (buf[0] >> 4) == 4) {                 /* IPv4 */
            int ihl = (buf[0] & 0x0f) * 4;
            if (ihl >= 20 && n >= ihl + 8 && buf[9] == 17) { /* UDP */
                uint32_t src, dst; memcpy(&src, buf + 12, 4); memcpy(&dst, buf + 16, 4);
                uint16_t sport = (buf[ihl] << 8) | buf[ihl + 1];
                uint16_t dport = (buf[ihl + 2] << 8) | buf[ihl + 3];
                /* Observe a live bsdrX LAN session (both ends private, a pairing port in play). */
                int is_pair = 0;
                for (size_t k = 0; k < sizeof pair_ports / sizeof pair_ports[0]; k++)
                    if (sport == pair_ports[k] || dport == pair_ports[k]) { is_pair = 1; break; }
                if (is_pair && is_private_v4(src) && is_private_v4(dst)) pair_note(src, dst);
                /* Mic uplink: a registered headset -> the cloud (public dst). Forward the raw IP packet. */
                if (!is_private_v4(dst)) {
                    flow_t *f = flow_find(src);
                    if (f && f->used) {
                        if (sendto(cfd, buf, (size_t)n, 0, (struct sockaddr *)&f->agent, sizeof f->agent) > 0) { f->fwd++; total_fwd++; }
                    }
                }
            }
        }

        /* 2) drain control messages from agents. */
        for (;;) {
            unsigned char m[2048];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(cfd, m, sizeof m, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (r <= 0) break;
            handle_ctrl(cfd, m, (int)r, &from);
        }

        /* 3) periodic: beacon, flow expiry, substitution fail-safe. */
        uint64_t t = now_ms();
        if (t - last_hello > 2000) { send_hello(cfd, port); last_hello = t; }
        for (int i = 0; i < MAX_FLOWS; i++) {
            flow_t *f = &g_flows[i];
            if (!f->used) continue;
            if (!f->is_static && t - f->last_reg_ms > FLOW_TTL_MS) {
                fprintf(stderr, "bsdr_micrelay: [%s] expired (%lu fwd)\n", ip_str(f->quest_be), f->fwd);
                flow_free(f); continue;
            }
            if (f->sub_on && t - f->last_ctrl_ms > SUB_TTL_MS) {
                fprintf(stderr, "bsdr_micrelay: [%s] substitute timed out -> passthrough\n", ip_str(f->quest_be));
                flow_sub_off(f);
            }
        }
    }

    for (int i = 0; i < MAX_FLOWS; i++) if (g_flows[i].used) flow_free(&g_flows[i]);
    mc_cap_close(cap);
    close(cfd);
    fprintf(stderr, "bsdr_micrelay: stopped (%lu forwarded)\n", total_fwd);
    return 0;
}
