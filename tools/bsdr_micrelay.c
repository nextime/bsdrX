/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* bsdr_micrelay — router-side owner-mic relay for bsdrX.
 *
 * The Bigscreen headset sends its microphone only to the Bigscreen cloud (as plain Opus RTP);
 * there is no LAN mic upload. On a WIRED LAN, bsdrX can ARP-spoof to see that stream, but over
 * WiFi it can't (the AP bridges per-station traffic). The router, however, IS in the path for
 * every packet the headset sends. Run this tiny relay on the router: it captures the headset's
 * outbound UDP with libpcap and forwards the raw IPv4 packets to bsdrX, which decodes them via
 * its normal owner-mic path (bsdr_agent --sniff-remote <port>). Because it captures the headset's
 * OWN uplink, the result is the owner's voice only — no room mixing, no MITM, no monitor mode.
 *
 * Build (host):   make micrelay
 * Build (router): cross-compile with the router toolchain + libpcap, e.g.
 *   mipsel-openwrt-linux-gcc -O2 -Iinclude -DBSDR_HAVE_PCAP=1 \
 *       tools/bsdr_micrelay.c src/micsniff_capture.c -lpcap -o bsdr_micrelay
 *
 * Usage:  bsdr_micrelay --iface <if> --quest <headset-ip> --to <bsdrx-host>:<port>
 */
#include "micsniff_capture.h"

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
#include <netinet/ip.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ---- cloud voice SUBSTITUTION (driven by bsdrX) ----
 * bsdrX sends magic-tagged messages back over our socket: 'C' control (mode + cloud dst) and 'A' a
 * modified RTP packet to forward to the cloud in place of the Quest's original. In substitute mode we
 * (a) iptables-DROP the Quest->cloud transit flow so the router stops forwarding the originals, and
 * (b) send bsdrX's modified RTP to the cloud from our own socket (OUTPUT chain, so the DROP — which is
 * on FORWARD — never touches it, and the cloud SFU sees one consistent source). The relay never
 * decodes/encodes: all codec + voice-change work stays in bsdrX. */
#define RLY_MAGIC "BSRL"

static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int  g_sub = 0;                 /* substitute mode active */
static int  g_drop = 0;                /* the FORWARD DROP rule is installed */
static char g_quest[64], g_cloud[64];  /* the flow currently dropped */
static int  g_cport = 0;
static int  g_cloud_fd = -1;           /* UDP socket to the cloud (legacy 'A' path only) */
static int  g_raw_fd = -1;             /* IP_HDRINCL raw socket: inject the modified datagram with
                                        * src=Quest so it reuses the Quest's NAT/conntrack flow (the
                                        * only source mediasoup's comedia latch keeps accepting) */

static void raw_open(void) {
    if (g_raw_fd >= 0) return;
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);   /* IPPROTO_RAW => IP_HDRINCL on */
    if (fd < 0) { fprintf(stderr, "bsdr_micrelay: raw socket failed (need root): %s\n", strerror(errno)); return; }
    int one = 1; (void)setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof one);
    g_raw_fd = fd;
}

static void drop_del(void) {
    if (!g_drop) return;
    char c[256];
    snprintf(c, sizeof c, "iptables -D FORWARD -s %s -d %s -p udp --dport %d -j DROP 2>/dev/null",
             g_quest, g_cloud, g_cport);
    if (system(c) != 0) { /* best effort */ }
    g_drop = 0;
}
static void drop_add(const char *quest, const char *cloud, int port) {
    if (g_drop) {
        if (!strcmp(g_quest, quest) && !strcmp(g_cloud, cloud) && g_cport == port) return;  /* unchanged */
        drop_del();
    }
    snprintf(g_quest, sizeof g_quest, "%s", quest);
    snprintf(g_cloud, sizeof g_cloud, "%s", cloud);
    g_cport = port;
    char c[256];
    snprintf(c, sizeof c, "iptables -I FORWARD -s %s -d %s -p udp --dport %d -j DROP 2>/dev/null",
             quest, cloud, port);
    if (system(c) == 0) { g_drop = 1; fprintf(stderr, "bsdr_micrelay: substitute ON (drop %s->%s:%d)\n", quest, cloud, port); }
    else fprintf(stderr, "bsdr_micrelay: WARNING iptables DROP failed (need root; is iptables present?)\n");
}
static void cloud_reopen(uint32_t cloud_be, int port) {
    if (g_cloud_fd >= 0) { close(g_cloud_fd); g_cloud_fd = -1; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = cloud_be; a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return; }  /* stable src port */
    g_cloud_fd = fd;
}

static void usage(const char *p) {
    fprintf(stderr,
        "bsdr_micrelay — forward a Bigscreen headset's mic RTP to bsdrX from the router.\n\n"
        "  %s --iface <if> --quest <headset-ip> --to <bsdrx-host>:<port>\n\n"
        "  --iface   capture interface (the router LAN/bridge the headset is on, e.g. br-lan)\n"
        "  --quest   the headset's IPv4 address (only its packets are forwarded)\n"
        "  --to      bsdrX host and UDP port it listens on (bsdr_agent --sniff-remote <port>)\n",
        p);
}

int main(int argc, char **argv) {
    const char *iface = NULL, *quest = NULL, *to = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc) iface = argv[++i];
        else if (!strcmp(argv[i], "--quest") && i + 1 < argc) quest = argv[++i];
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    }
    if (!iface || !quest || !to) { usage(argv[0]); return 2; }

    /* parse host:port (split on the LAST ':' so IPv4 hosts are unambiguous) */
    char host[128]; int port = 0;
    const char *colon = strrchr(to, ':');
    if (!colon || colon == to) { fprintf(stderr, "bsdr_micrelay: --to must be host:port\n"); return 2; }
    size_t hn = (size_t)(colon - to);
    if (hn >= sizeof host) hn = sizeof host - 1;
    memcpy(host, to, hn); host[hn] = 0;
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) { fprintf(stderr, "bsdr_micrelay: bad port in --to\n"); return 2; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET; dst.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { fprintf(stderr, "bsdr_micrelay: bad host %s\n", host); close(fd); return 2; }
    if (connect(fd, (struct sockaddr *)&dst, sizeof dst) != 0) { perror("connect"); close(fd); return 1; }

    char err[256] = "";
    mc_cap *cap = mc_cap_open(iface, quest, err, sizeof err);
    if (!cap) { fprintf(stderr, "bsdr_micrelay: capture on %s failed: %s\n(need root / CAP_NET_RAW)\n", iface, err); close(fd); return 1; }
    fprintf(stderr, "bsdr_micrelay: capturing %s (quest %s) -> %s:%d\n", iface, quest, host, port);

    unsigned char buf[2048];
    unsigned long fwd = 0, subbed = 0;
    uint64_t last_ctrl = 0;
    while (!g_stop) {
        int n = mc_cap_next(cap, buf, sizeof buf, 20);
        if (n < 0) break;
        if (n > 0) { if (send(fd, buf, (size_t)n, 0) > 0) fwd++; }   /* forward the original to bsdrX */

        /* Drain control + modified-audio messages from bsdrX (our socket is connect()ed to it). */
        for (;;) {
            unsigned char m[2048];
            ssize_t r = recv(fd, m, sizeof m, MSG_DONTWAIT);
            if (r <= 0) break;
            if (r < 5 || memcmp(m, RLY_MAGIC, 4) != 0) continue;
            if (m[4] == 'C' && r >= 12) {                 /* control: mode + cloud dst */
                int mode = m[5];
                uint32_t cloud_be; memcpy(&cloud_be, m + 6, 4);
                int port = (m[10] << 8) | m[11];
                char cs[64]; struct in_addr ia; ia.s_addr = cloud_be;
                snprintf(cs, sizeof cs, "%s", inet_ntoa(ia));
                if (mode == 1) {
                    if (!g_sub || g_cloud_fd < 0 || strcmp(g_cloud, cs) != 0 || g_cport != port)
                        cloud_reopen(cloud_be, port);
                    raw_open();                     /* for the 'F' (full-datagram) inject path */
                    drop_add(quest, cs, port);
                    g_sub = 1; last_ctrl = now_ms();
                } else {
                    if (g_sub) { drop_del(); g_sub = 0; fprintf(stderr, "bsdr_micrelay: substitute OFF\n"); }
                }
            } else if (m[4] == 'F' && g_sub && g_raw_fd >= 0 && r > 5 + 20) {
                /* Full modified IPv4 datagram (src=Quest, dst=cloud). RAW-inject it: the kernel keeps our
                 * source and conntrack/masquerade re-uses the Quest flow's NAT mapping, so the cloud sees
                 * the same source tuple as the original -> comedia accepts it (no mute). */
                const uint8_t *dg = m + 5; int dglen = (int)(r - 5);
                struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
                dst.sin_family = AF_INET; memcpy(&dst.sin_addr.s_addr, dg + 16, 4);  /* dst IP from header */
                if (sendto(g_raw_fd, dg, (size_t)dglen, 0, (struct sockaddr *)&dst, sizeof dst) > 0) subbed++;
            } else if (m[4] == 'A' && g_sub && g_cloud_fd >= 0) {   /* legacy: modified RTP -> UDP socket */
                if (send(g_cloud_fd, m + 5, (size_t)(r - 5), 0) > 0) subbed++;
            }
        }
        /* Fail-safe: if bsdrX stops driving substitution, restore passthrough so the mic isn't muted. */
        if (g_sub && now_ms() - last_ctrl > 2000) { drop_del(); g_sub = 0; fprintf(stderr, "bsdr_micrelay: substitute timed out -> passthrough\n"); }
    }
    drop_del();
    if (g_cloud_fd >= 0) close(g_cloud_fd);
    if (g_raw_fd >= 0) close(g_raw_fd);
    mc_cap_close(cap);
    close(fd);
    fprintf(stderr, "bsdr_micrelay: stopped (%lu forwarded, %lu substituted)\n", fwd, subbed);
    return 0;
}
