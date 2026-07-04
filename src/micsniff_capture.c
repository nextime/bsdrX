/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Owner-mic sniffer capture backend — libpcap (portable) or AF_PACKET (Linux fallback). */
#include "micsniff_capture.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* ============================================================ libpcap path */
#if defined(BSDR_HAVE_PCAP)

#include <pcap.h>

struct mc_cap {
    pcap_t *pd;
    int     dlt;
    char    backend[16];
};

/* Offset from the start of a captured frame to the IPv4 header, per DLT. -1 = not IPv4/unsupported. */
static int l2_offset(int dlt, const unsigned char *d, int caplen) {
    switch (dlt) {
    case DLT_EN10MB: {                              /* Ethernet (incl. Wi-Fi presented as EN10MB) */
        if (caplen < 14) return -1;
        int off = 14, et = (d[12] << 8) | d[13];
        if (et == 0x8100 || et == 0x88a8) {         /* 802.1Q / 802.1ad VLAN tag */
            if (caplen < 18) return -1;
            off = 18; et = (d[16] << 8) | d[17];
        }
        return et == 0x0800 ? off : -1;             /* IPv4 only */
    }
    case DLT_NULL:                                  /* BSD loopback: 4-byte host-order AF */
    case DLT_LOOP:  return caplen >= 4 ? 4 : -1;
    case DLT_RAW:   return 0;                        /* raw IP */
#ifdef DLT_LINUX_SLL
    case DLT_LINUX_SLL:  return caplen >= 16 ? 16 : -1;
#endif
#ifdef DLT_LINUX_SLL2
    case DLT_LINUX_SLL2: return caplen >= 20 ? 20 : -1;
#endif
    default: return -1;
    }
}

mc_cap *mc_cap_open(const char *iface, const char *quest_ip, char *err, size_t errlen) {
    char eb[PCAP_ERRBUF_SIZE] = "";
    pcap_t *pd = pcap_create(iface, eb);
    if (!pd) { snprintf(err, errlen, "pcap_create(%s): %s", iface, eb); return NULL; }
    pcap_set_snaplen(pd, 2048);
    pcap_set_promisc(pd, 1);
    pcap_set_timeout(pd, 300);
    pcap_set_immediate_mode(pd, 1);                 /* deliver ASAP — low latency for audio */
    int act = pcap_activate(pd);
    if (act < 0) { snprintf(err, errlen, "pcap_activate(%s): %s", iface, pcap_geterr(pd));
                   pcap_close(pd); return NULL; }

    struct bpf_program fp;
    char filt[128];
    snprintf(filt, sizeof filt, "udp and src host %s", quest_ip);
    if (pcap_compile(pd, &fp, filt, 1, PCAP_NETMASK_UNKNOWN) == 0) {
        pcap_setfilter(pd, &fp);
        pcap_freecode(&fp);
    } else {
        snprintf(err, errlen, "pcap_compile: %s", pcap_geterr(pd));  /* non-fatal: filter in decoder */
    }

    mc_cap *c = calloc(1, sizeof *c);
    if (!c) { pcap_close(pd); snprintf(err, errlen, "oom"); return NULL; }
    c->pd = pd; c->dlt = pcap_datalink(pd);
    snprintf(c->backend, sizeof c->backend, "libpcap");
    return c;
}

int mc_cap_next(mc_cap *c, unsigned char *buf, int maxlen, int timeout_ms) {
    (void)timeout_ms;                               /* set via pcap_set_timeout at open */
    struct pcap_pkthdr *h; const unsigned char *d;
    int r = pcap_next_ex(c->pd, &h, &d);
    if (r == 0) return 0;                           /* timeout */
    if (r < 0) return -1;                           /* error / EOF */
    int off = l2_offset(c->dlt, d, (int)h->caplen);
    if (off < 0 || off >= (int)h->caplen) return 0;
    int n = (int)h->caplen - off;
    if (n > maxlen) n = maxlen;
    memcpy(buf, d + off, (size_t)n);
    return n;
}

int mc_cap_inject(mc_cap *c, const unsigned char *frame, int len) {
    return pcap_sendpacket(c->pd, frame, len);      /* raw Ethernet frame */
}

const char *mc_cap_backend(const mc_cap *c) { return c->backend; }

void mc_cap_close(mc_cap *c) { if (!c) return; if (c->pd) pcap_close(c->pd); free(c); }

/* ============================================================ AF_PACKET path (Linux) */
#elif defined(__linux__)

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/filter.h>

struct mc_cap {
    int  sock;                                      /* SOCK_DGRAM ETH_P_IP: delivers bare IP */
    int  inj;                                       /* SOCK_RAW for ARP injection (lazy) */
    int  ifindex;
    char backend[16];
};

static void attach_bpf(int sock, uint32_t quest_be) {   /* keep only UDP from the Quest */
    uint32_t q = ntohl(quest_be);
    struct sock_filter code[] = {
        { 0x30, 0, 0, 0x00000009 }, { 0x15, 0, 3, 0x00000011 },
        { 0x20, 0, 0, 0x0000000c }, { 0x15, 0, 1, q },
        { 0x06, 0, 0, 0x00040000 }, { 0x06, 0, 0, 0x00000000 },
    };
    struct sock_fprog prog = { .len = 6, .filter = code };
    (void)setsockopt(sock, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog);
}

mc_cap *mc_cap_open(const char *iface, const char *quest_ip, char *err, size_t errlen) {
    int ifidx = (int)if_nametoindex(iface);
    if (ifidx == 0) { snprintf(err, errlen, "unknown interface %s", iface); return NULL; }
    int s = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (s < 0) { snprintf(err, errlen, "AF_PACKET socket: %s (need root/CAP_NET_RAW)", strerror(errno)); return NULL; }
    attach_bpf(s, inet_addr(quest_ip));
    struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET; sll.sll_protocol = htons(ETH_P_IP); sll.sll_ifindex = ifidx;
    if (bind(s, (struct sockaddr *)&sll, sizeof sll) != 0) {
        snprintf(err, errlen, "bind %s: %s", iface, strerror(errno)); close(s); return NULL;
    }
    struct packet_mreq mr; memset(&mr, 0, sizeof mr);
    mr.mr_ifindex = ifidx; mr.mr_type = PACKET_MR_PROMISC;
    (void)setsockopt(s, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    mc_cap *c = calloc(1, sizeof *c);
    if (!c) { close(s); snprintf(err, errlen, "oom"); return NULL; }
    c->sock = s; c->inj = -1; c->ifindex = ifidx;
    snprintf(c->backend, sizeof c->backend, "af_packet");
    return c;
}

int mc_cap_next(mc_cap *c, unsigned char *buf, int maxlen, int timeout_ms) {
    (void)timeout_ms;
    ssize_t n = recvfrom(c->sock, buf, (size_t)maxlen, 0, NULL, NULL);   /* already bare IP */
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    return (int)n;
}

int mc_cap_inject(mc_cap *c, const unsigned char *frame, int len) {
    if (c->inj < 0) { c->inj = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)); if (c->inj < 0) return -1; }
    struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET; sll.sll_ifindex = c->ifindex;
    sll.sll_halen = 6; memcpy(sll.sll_addr, frame, 6);   /* dest MAC = frame[0..5] */
    return sendto(c->inj, frame, (size_t)len, 0, (struct sockaddr *)&sll, sizeof sll) < 0 ? -1 : 0;
}

const char *mc_cap_backend(const mc_cap *c) { return c->backend; }

void mc_cap_close(mc_cap *c) { if (!c) return; if (c->sock >= 0) close(c->sock); if (c->inj >= 0) close(c->inj); free(c); }

/* ============================================================ unsupported */
#else

mc_cap *mc_cap_open(const char *iface, const char *quest_ip, char *err, size_t errlen) {
    (void)iface; (void)quest_ip;
    snprintf(err, errlen, "no capture backend (build without libpcap on a non-Linux platform)");
    return NULL;
}
int mc_cap_next(mc_cap *c, unsigned char *buf, int maxlen, int timeout_ms) { (void)c;(void)buf;(void)maxlen;(void)timeout_ms; return -1; }
int mc_cap_inject(mc_cap *c, const unsigned char *frame, int len) { (void)c;(void)frame;(void)len; return -1; }
const char *mc_cap_backend(const mc_cap *c) { (void)c; return "none"; }
void mc_cap_close(mc_cap *c) { (void)c; }

#endif
