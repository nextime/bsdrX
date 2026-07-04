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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

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
    unsigned long fwd = 0;
    while (!g_stop) {
        int n = mc_cap_next(cap, buf, sizeof buf, 300);
        if (n < 0) break;
        if (n > 0) { if (send(fd, buf, (size_t)n, 0) > 0) fwd++; }
    }
    mc_cap_close(cap);
    close(fd);
    fprintf(stderr, "bsdr_micrelay: stopped (%lu packets forwarded)\n", fwd);
    return 0;
}
