/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Owner-mic sniffer — intercept the Quest's outbound room mic (plain Opus RTP to the mediasoup
 * cloud) off the LAN and feed the owner-only voice into a dedicated virtual microphone. Why:
 * see include/bsdr/micsniff.h.
 *
 * PRIVILEGE SEPARATION (POSIX: Linux + macOS). Packet capture / ARP / routing knobs need root;
 * decoding does not. So a short-lived HELPER process (the agent re-execs itself via sudo with
 * --sniff-helper) does ONLY the privileged work: it captures with mc_cap (libpcap or AF_PACKET)
 * and STREAMS the bare IP packets to the unprivileged parent over a Unix SOCK_SEQPACKET socket;
 * for MITM it also ARP-poisons and flips the OS forwarding knobs. The parent — which owns the
 * user's audio session — decodes and plays into the virtual mic. When the parent detaches
 * (closes the socket) the helper heals ARP, restores the knobs, and exits.
 *
 * WINDOWS. No fork/sudo model: capture (Npcap) and the VB-CABLE virtual mic both assume the agent
 * runs elevated, so the sniffer captures in-process (no helper). MITM is done in-process too: ARP
 * poisoning via Npcap injection, neighbour MACs via SendARP, our MAC/gateway via GetAdaptersAddresses,
 * and IP forwarding toggled at runtime via EnableRouter/UnenableRouter (iphlpapi). Prereqs are
 * surfaced at runtime. */
#include "bsdr/micsniff.h"
#include "bsdr/log.h"

#if (defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__)) && defined(BSDR_HAVE_AUDIO)

#include "bsdr/audio.h"
#include "bsdr/voicefx.h"
#include "bsdr/platform.h"
#include "micsniff_capture.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <opus/opus.h>

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)
#define POSIX_HELPER 1
#else
#define POSIX_HELPER 0
#endif

#if defined(__ANDROID__)
/* Android: relay-only owner mic (no local capture). The shared handle_ip below uses BSD sockets +
 * inet_ntoa, so pull those in (the POSIX_HELPER include block that normally provides them is off). */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#endif

#if POSIX_HELPER
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#if defined(__linux__)
#include <linux/if_packet.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#include <sys/sysctl.h>
#endif
#endif /* POSIX_HELPER */

#if defined(_WIN32)
#include <winsock2.h>          /* inet_addr / ntohl / htons / inet_ntoa / INADDR_NONE */
#include <ws2tcpip.h>
#include <iphlpapi.h>          /* SendARP / GetAdaptersAddresses / EnableRouter (MITM) */
#endif

#if defined(__APPLE__)
#define OWNER_SINK   "BlackHole"          /* render into the BlackHole loopback device */
#elif defined(_WIN32)
#define OWNER_SINK   BSDR_MIC_DEVICE_NAME  /* VB-CABLE, renamed "BSRD_Mic" by the installer */
#else
#define OWNER_SINK   "bsdr_ownersink"      /* PulseAudio null-sink we create */
#endif
#define OWNER_SOURCE "bsdr_quest_owner_mic"
#define OWNER_DESC   "BSDR-Quest-OwnerMic"

struct bsdr_micsniff {
    char        quest_ip[64];
    char        iface[64];
    char        gw_ip[64];
    uint32_t    quest_be;
    int         mitm;
    int         remote_port;   /* >0: receive captured IP packets from a router companion over UDP
                                * instead of capturing locally (no helper/root needed). */

    bsdr_thread *thr;
    volatile int stop;

    /* POSIX: data/liveness socket to the helper. WINDOWS: -1 (in-process capture). */
    int         data_fd;
    long        helper_pid;                         /* pid_t as long; -1 if none */
    mc_cap     *cap;                                /* WINDOWS in-process capture */

    int         sink_mod, src_mod;
    bsdr_audio_player *player;
    OpusDecoder *dec;

    /* realtime voice changer (gender shift) applied to the decoded owner voice before it reaches the
     * virtual mic / the cloud / the command tap. 0 = off. */
    bsdr_voicefx *fx;
    bsdr_voicefx_params fxp;

    /* optional voice-command tap (set by the agent while computer-control is on) */
    bsdr_micsniff_pcm_cb pcm_cb;
    void        *pcm_user;

    int         have_flow;
    uint32_t    flow_dst_be;
    uint16_t    flow_dport;
    uint32_t    flow_ssrc;
    uint32_t    last_frame_id;
    int         have_last;
    long        decoded;

#if defined(_WIN32)
    /* MITM state (Windows runs the ARP poisoner in-process, no privileged helper) */
    uint8_t     w_our_mac[6], w_quest_mac[6], w_gw_mac[6];
    uint32_t    w_our_ip, w_gw_be;
    int         w_have_mine, w_have_qmac, w_have_gmac, w_router_on;
    uint64_t    w_last_arp;
    void       *w_router_handle, *w_router_ovl;   /* HANDLE + OVERLAPPED* from EnableRouter */
#endif
};

/* ---------------------------------------------------------- shared helpers */

static int is_private_v4(uint32_t be) {
    uint32_t h = ntohl(be);
    uint8_t a = (h >> 24) & 0xff, b = (h >> 16) & 0xff;
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 1;
    if (a == 192 && b == 168) return 1;
    if (a == 169 && b == 254) return 1;
    if (a == 100 && b >= 64 && b <= 127) return 1;
    if (a == 127) return 1;
    if (a >= 224) return 1;
    return 0;
}

/* Build a 42-byte Ethernet+ARP frame (op 1 = request, 2 = reply). Pure byte layout, shared by the
 * POSIX helper and the Windows in-process MITM. */
#define BSDR_ARP_REPLY 2
static void build_arp(uint8_t f[42], int op, const uint8_t smac[6], uint32_t sip,
                      const uint8_t tmac[6], uint32_t tip) {
    memcpy(f + 0, tmac, 6); memcpy(f + 6, smac, 6);
    f[12] = 0x08; f[13] = 0x06; f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00;
    f[18] = 6; f[19] = 4; f[20] = (uint8_t)(op >> 8); f[21] = (uint8_t)op;
    memcpy(f + 22, smac, 6); memcpy(f + 28, &sip, 4);
    memcpy(f + 32, tmac, 6); memcpy(f + 38, &tip, 4);
}

/* Realtime voice change on the decoded owner voice (in place), lazily creating/reconfiguring the DSP
 * when the gender knob changes. A gender of 0 is a no-op. 48 kHz mono, as decoded from the Opus. */
static void micsniff_apply_fx(struct bsdr_micsniff *s, int16_t *pcm, int frames) {
    if (!s->fxp.gender && !s->fxp.robot && !s->fxp.echo && !s->fxp.whisper) return;
    if (!s->fx) { s->fx = bsdr_voicefx_new(48000); if (!s->fx) return; }
    bsdr_voicefx_set_params(s->fx, &s->fxp);
    bsdr_voicefx_process(s->fx, pcm, frames);
}

/* Parse one captured IPv4 datagram: strip the 8B [ssrc][frame_id] trailer, dedupe the 2x send,
 * lock the first mono-Opus flow, decode, and play out. (parent / in-process, unprivileged) */
static void handle_ip(struct bsdr_micsniff *s, const uint8_t *ip, int len) {
    if (len < 20) return;
    if ((ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || len < ihl + 8) return;
    if (ip[9] != 17) return;                        /* UDP */
    uint32_t src_be, dst_be;
    memcpy(&src_be, ip + 12, 4);
    memcpy(&dst_be, ip + 16, 4);
    if (src_be != s->quest_be) return;
    if (is_private_v4(dst_be)) return;              /* LAN/discovery, not the cloud */

    const uint8_t *udp = ip + ihl;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    int ulen = (udp[4] << 8) | udp[5];
    if (ulen < 8 || ihl + ulen > len) ulen = len - ihl;
    const uint8_t *rtp = udp + 8;
    int rlen = ulen - 8;
    if (rlen < 12) return;

    if ((rtp[0] >> 6) != 2) return;                 /* RTP v2 */
    int hlen = 12 + 4 * (rtp[0] & 0x0f);
    if (rtp[0] & 0x10) {
        if (rlen < hlen + 4) return;
        int extw = (rtp[hlen + 2] << 8) | rtp[hlen + 3];
        hlen += 4 + 4 * extw;
    }
    if (rlen <= hlen + 8) return;
    uint32_t ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) |
                    ((uint32_t)rtp[10] << 8) | rtp[11];

    const uint8_t *pl = rtp + hlen;
    int plen = rlen - hlen;
    int olen = plen - 8;
    const uint8_t *tr = pl + olen;
    uint32_t frame_id = (uint32_t)tr[4] | ((uint32_t)tr[5] << 8) |
                        ((uint32_t)tr[6] << 16) | ((uint32_t)tr[7] << 24);

    if (!s->have_flow) {
        if (pl[0] & 0x04) return;                   /* Opus TOC stereo → not the mono mic */
        int16_t probe[5760];
        int fr = opus_decode(s->dec, pl, olen, probe, 5760, 0);
        if (fr <= 0) return;
        s->flow_dst_be = dst_be; s->flow_dport = dport; s->flow_ssrc = ssrc;
        s->have_flow = 1;
        s->last_frame_id = frame_id; s->have_last = 1;
        struct in_addr a; a.s_addr = dst_be;
        BSDR_INFO("bsdr.micsniff", "locked owner-mic flow: %s -> %s:%u ssrc=%u (mono Opus)",
                  s->quest_ip, inet_ntoa(a), dport, ssrc);
        micsniff_apply_fx(s, probe, fr);
        if (s->player) bsdr_audio_player_push(s->player, probe, fr);
        if (s->pcm_cb) s->pcm_cb(s->pcm_user, probe, fr, 1);
        s->decoded++;
        return;
    }
    if (dst_be != s->flow_dst_be || dport != s->flow_dport || ssrc != s->flow_ssrc) return;
    if (s->have_last && frame_id == s->last_frame_id) return;    /* duplicate (2x) */
    s->last_frame_id = frame_id; s->have_last = 1;

    int16_t pcm[5760];
    int fr = opus_decode(s->dec, pl, olen, pcm, 5760, 0);
    if (fr <= 0) return;
    micsniff_apply_fx(s, pcm, fr);
    if (s->player) bsdr_audio_player_push(s->player, pcm, fr);
    if (s->pcm_cb) s->pcm_cb(s->pcm_user, pcm, fr, 1);
    if ((++s->decoded % 500) == 0)
        BSDR_DEBUG("bsdr.micsniff", "owner-mic: %ld frames decoded", s->decoded);
}

/* ========================================================= POSIX (Linux/macOS) */
#if POSIX_HELPER

#define PKT_MAX 2048

/* ---- MITM platform bits (helper only) ---- */

/* Interface MAC + IPv4 + index. Portable via getifaddrs (+ SIOCGIFHWADDR / AF_LINK for the MAC). */
static int iface_info(const char *iface, uint8_t mac[6], uint32_t *ip_be, int *ifindex) {
    *ifindex = (int)if_nametoindex(iface);
    if (*ifindex == 0) return -1;
    int got_ip = 0, got_mac = 0;
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || strcmp(p->ifa_name, iface) != 0) continue;
            if (p->ifa_addr->sa_family == AF_INET) {
                *ip_be = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr; got_ip = 1;
            }
#if defined(__APPLE__)
            else if (p->ifa_addr->sa_family == AF_LINK) {
                struct sockaddr_dl *dl = (struct sockaddr_dl *)p->ifa_addr;
                if (dl->sdl_alen == 6) { memcpy(mac, LLADDR(dl), 6); got_mac = 1; }
            }
#endif
        }
        freeifaddrs(ifa);
    }
#if defined(__linux__)
    { int fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd >= 0) { struct ifreq r; memset(&r, 0, sizeof r); snprintf(r.ifr_name, IFNAMSIZ, "%s", iface);
                     if (ioctl(fd, SIOCGIFHWADDR, &r) == 0) { memcpy(mac, r.ifr_hwaddr.sa_data, 6); got_mac = 1; }
                     close(fd); } }
#endif
    return (got_ip && got_mac) ? 0 : -1;
}

/* Resolve a neighbour's MAC: prime the cache with a stray UDP packet, then scan `arp -n <ip>`
 * output for a MAC token (works on both Linux and macOS despite different table formats). */
static int resolve_mac(uint32_t ip_be, uint8_t out[6]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) { struct sockaddr_in d; memset(&d, 0, sizeof d);
                   d.sin_family = AF_INET; d.sin_addr.s_addr = ip_be; d.sin_port = htons(9);
                   (void)sendto(fd, "", 0, 0, (struct sockaddr *)&d, sizeof d); close(fd); }
    char ips[64]; struct in_addr a; a.s_addr = ip_be; snprintf(ips, sizeof ips, "%s", inet_ntoa(a));
    for (int tries = 0; tries < 20; tries++) {
        char cmd[128]; snprintf(cmd, sizeof cmd, "arp -n %s 2>/dev/null", ips);
        FILE *f = popen(cmd, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                for (char *q = line; *q; q++) {
                    unsigned m[6];
                    if (sscanf(q, "%x:%x:%x:%x:%x:%x", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6 &&
                        (m[0]|m[1]|m[2]|m[3]|m[4]|m[5])) {
                        for (int i = 0; i < 6; i++) out[i] = (uint8_t)m[i];
                        pclose(f); return 0;
                    }
                }
            }
            pclose(f);
        }
        bsdr_sleep_ms(100);
    }
    return -1;
}

/* OS forwarding + redirect knobs. Returns the previous value (or -1) so we can restore it. */
#if defined(__linux__)
static int proc_read(const char *p) { FILE *f = fopen(p, "r"); if (!f) return -1; int v = -1; if (fscanf(f,"%d",&v)!=1) v=-1; fclose(f); return v; }
static void proc_write(const char *p, int v) { FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); } }
static int  get_forwarding(void) { return proc_read("/proc/sys/net/ipv4/ip_forward"); }
static void set_forwarding(int v){ proc_write("/proc/sys/net/ipv4/ip_forward", v); }
static int  get_redirects(void) { return proc_read("/proc/sys/net/ipv4/conf/all/send_redirects"); }
static void set_redirects(int v){ proc_write("/proc/sys/net/ipv4/conf/all/send_redirects", v); }
#elif defined(__APPLE__)
static int sysctl_get(const char *n){ int v=0; size_t l=sizeof v; return sysctlbyname(n,&v,&l,NULL,0)==0?v:-1; }
static void sysctl_set(const char *n,int v){ (void)sysctlbyname(n,NULL,NULL,&v,sizeof v); }
static int  get_forwarding(void){ return sysctl_get("net.inet.ip.forwarding"); }
static void set_forwarding(int v){ sysctl_set("net.inet.ip.forwarding", v); }
static int  get_redirects(void){ return sysctl_get("net.inet.ip.redirect"); }
static void set_redirects(int v){ sysctl_set("net.inet.ip.redirect", v); }
#endif

/* ---- helper (root): capture pump + optional MITM ---- */

int bsdr_micsniff_helper_main(int argc, char **argv) {
    const char *sockpath = NULL, *quest = NULL, *iface = NULL, *gw = NULL;
    int mitm = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--sniff-sock")  && i + 1 < argc) sockpath = argv[++i];
        else if (!strcmp(argv[i], "--quest_ip")    && i + 1 < argc) quest = argv[++i];
        else if (!strcmp(argv[i], "--sniff-iface") && i + 1 < argc) iface = argv[++i];
        else if (!strcmp(argv[i], "--sniff-gw")    && i + 1 < argc) gw = argv[++i];
        else if (!strcmp(argv[i], "--sniff-mitm")) mitm = 1;
    }
    if (!sockpath || !quest || !iface) { fprintf(stderr, "micsniff-helper: missing args\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);

    int c = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (c < 0) { perror("micsniff-helper: socket"); return 2; }
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX; snprintf(un.sun_path, sizeof un.sun_path, "%s", sockpath);
    if (connect(c, (struct sockaddr *)&un, sizeof un) != 0) { perror("micsniff-helper: connect"); close(c); return 2; }

    char err[256] = "";
    mc_cap *cap = mc_cap_open(iface, quest, err, sizeof err);
    if (!cap) { fprintf(stderr, "micsniff-helper: %s\n", err); uint8_t st = 1; (void)!write(c, &st, 1); close(c); return 3; }

    uint32_t quest_be = inet_addr(quest);
    int ifidx = -1, ip_fwd_was = -1, redir_was = -1;
    uint8_t our_mac[6], quest_mac[6], gw_mac[6]; uint32_t our_ip = 0, gw_be = 0;
    int have_qmac = 0, have_gmac = 0, have_mine = 0;
    if (mitm && gw) {
        gw_be = inet_addr(gw);
        have_mine = (iface_info(iface, our_mac, &our_ip, &ifidx) == 0);
        have_qmac = (resolve_mac(quest_be, quest_mac) == 0);
        have_gmac = (resolve_mac(gw_be, gw_mac) == 0);
        ip_fwd_was = get_forwarding(); redir_was = get_redirects();
        set_forwarding(1); set_redirects(0);
        if (!have_mine || !have_qmac || !have_gmac)
            fprintf(stderr, "micsniff-helper: MITM degraded (mine=%d qmac=%d gmac=%d)\n", have_mine, have_qmac, have_gmac);
    }

    uint8_t st = 0; if (write(c, &st, 1) != 1) { mc_cap_close(cap); close(c); return 0; }   /* ready */
    fcntl(c, F_SETFL, O_NONBLOCK);

    uint64_t last_arp = 0;
    unsigned char pkt[PKT_MAX];
    for (;;) {
        if (mitm && have_mine) {
            uint64_t now = bsdr_now_ms();
            if (now - last_arp >= 1000) {           /* ~1 s ARP cadence */
                uint8_t f[42];
                if (have_qmac) { build_arp(f, ARPOP_REPLY, our_mac, gw_be, quest_mac, quest_be); mc_cap_inject(cap, f, 42); }
                if (have_gmac) { build_arp(f, ARPOP_REPLY, our_mac, quest_be, gw_mac, gw_be); mc_cap_inject(cap, f, 42); }
                last_arp = now;
            }
        }
        int n = mc_cap_next(cap, pkt, sizeof pkt, 300);
        if (n < 0) break;
        if (n > 0) { if (send(c, pkt, (size_t)n, 0) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break; }
        char b; ssize_t r = recv(c, &b, 1, MSG_PEEK | MSG_DONTWAIT);   /* parent gone? */
        if (r == 0) break;
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
    }

    if (mitm && have_mine && have_qmac && have_gmac)   /* heal both caches */
        for (int i = 0; i < 3; i++) {
            uint8_t f[42];
            build_arp(f, ARPOP_REPLY, gw_mac, gw_be, quest_mac, quest_be); mc_cap_inject(cap, f, 42);
            build_arp(f, ARPOP_REPLY, quest_mac, quest_be, gw_mac, gw_be); mc_cap_inject(cap, f, 42);
            bsdr_sleep_ms(150);
        }
    if (mitm) { if (ip_fwd_was >= 0) set_forwarding(ip_fwd_was); if (redir_was >= 0) set_redirects(redir_was); }
    mc_cap_close(cap);
    close(c);
    return 0;
}

/* ---- parent (user): spawn helper, read streamed packets, decode ---- */

/* Fill iface/gw from the default route if not already set (unprivileged). */
static void default_route(char *iface, size_t ilen, char *gw, size_t glen) {
#if defined(__linux__)
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof line, f)) {              /* header */
        while (fgets(line, sizeof line, f)) {
            char ifn[64]; unsigned long dest = 1, gwv = 0;
            if (sscanf(line, "%63s %lx %lx", ifn, &dest, &gwv) < 3) continue;
            if (dest != 0) continue;
            if (!iface[0]) snprintf(iface, ilen, "%s", ifn);
            if (!gw[0]) { struct in_addr a; a.s_addr = (in_addr_t)gwv; snprintf(gw, glen, "%s", inet_ntoa(a)); }
            break;
        }
    }
    fclose(f);
#elif defined(__APPLE__)
    FILE *f = popen("route -n get default 2>/dev/null", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char v[128];
        if (!iface[0] && sscanf(line, " interface: %127s", v) == 1) snprintf(iface, ilen, "%s", v);
        else if (!gw[0] && sscanf(line, " gateway: %127s", v) == 1) snprintf(gw, glen, "%s", v);
    }
    pclose(f);
#endif
}

static void rendezvous_path(char *out, size_t n) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(out, n, "%s/bsdr-micsniff-%ld.sock", dir, (long)getpid());
}

static int self_path(char *out, size_t n) {
#if defined(__APPLE__)
    uint32_t sz = (uint32_t)n; extern int _NSGetExecutablePath(char*, uint32_t*);
    return _NSGetExecutablePath(out, &sz) == 0 ? 0 : -1;
#else
    ssize_t sl = readlink("/proc/self/exe", out, n - 1);
    if (sl <= 0) return -1;
    out[sl] = 0;
    return 0;
#endif
}

static int spawn_helper(struct bsdr_micsniff *s, const char *password, long *pid, int *data) {
    char self[512];
    if (self_path(self, sizeof self) != 0) { BSDR_ERROR("bsdr.micsniff", "cannot resolve own path"); return -1; }

    char path[100]; rendezvous_path(path, sizeof path); unlink(path);
    int lsn = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (lsn < 0) return -1;
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX; snprintf(un.sun_path, sizeof un.sun_path, "%s", path);
    if (bind(lsn, (struct sockaddr *)&un, sizeof un) != 0 || listen(lsn, 1) != 0) {
        BSDR_ERROR("bsdr.micsniff", "rendezvous bind: %s", strerror(errno)); close(lsn); return -1;
    }
    chmod(path, 0600);

    int already_root = (geteuid() == 0);
    int use_pw = (!already_root && password && password[0]);
    char *av[24]; int ac = 0;
    if (!already_root) { av[ac++] = "sudo"; if (use_pw) { av[ac++] = "-S"; av[ac++] = "-p"; av[ac++] = ""; } }
    av[ac++] = self; av[ac++] = "--sniff-helper";
    av[ac++] = "--sniff-sock";  av[ac++] = path;
    av[ac++] = "--quest_ip";    av[ac++] = s->quest_ip;
    av[ac++] = "--sniff-iface"; av[ac++] = s->iface;
    if (s->mitm && s->gw_ip[0]) { av[ac++] = "--sniff-gw"; av[ac++] = s->gw_ip; }
    if (s->mitm) av[ac++] = "--sniff-mitm";
    av[ac] = NULL;

    int pw_pipe[2] = { -1, -1 };
    if (use_pw && pipe(pw_pipe) != 0) { close(lsn); unlink(path); return -1; }
    pid_t child = fork();
    if (child < 0) { close(lsn); unlink(path); if (use_pw){close(pw_pipe[0]);close(pw_pipe[1]);} return -1; }
    if (child == 0) {
        close(lsn);
        if (use_pw) { dup2(pw_pipe[0], 0); close(pw_pipe[0]); close(pw_pipe[1]); }
        execvp(av[0], av); perror("micsniff: exec"); _exit(127);
    }
    if (use_pw) { close(pw_pipe[0]); char line[160]; int ln = snprintf(line, sizeof line, "%s\n", password);
                  if (write(pw_pipe[1], line, (size_t)ln) < 0) {} close(pw_pipe[1]); }

    int accept_ms = use_pw ? 25000 : (already_root ? 5000 : 90000);
    struct pollfd pfd = { .fd = lsn, .events = POLLIN };
    int fd = -1; uint8_t stbyte = 0xff;
    if (poll(&pfd, 1, accept_ms) > 0) {
        int aconn = accept(lsn, NULL, NULL);
        if (aconn >= 0) {
            if (recv(aconn, &stbyte, 1, 0) == 1 && stbyte == 0) fd = aconn;   /* ready */
            else close(aconn);
        }
    }
    close(lsn); unlink(path);
    if (fd < 0) {
        BSDR_ERROR("bsdr.micsniff", "privileged helper failed (sudo denied / wrong password / "
                   "no capture permission on %s)", s->iface);
        int status; waitpid(child, &status, 0); *pid = -1; return -1;
    }
    *pid = (long)child; *data = fd;
    return 0;
}

static void sniff_main(void *arg) {
    struct bsdr_micsniff *s = (struct bsdr_micsniff *)arg;
    BSDR_INFO("bsdr.micsniff", "%s-sniffing Quest %s room mic on %s -> %s",
              s->mitm ? "MITM" : "passive", s->quest_ip, s->iface, OWNER_DESC);
    unsigned char buf[PKT_MAX];
    long captured = 0; int warned = 0; uint64_t start = bsdr_now_ms();
    while (!s->stop) {
        struct pollfd pfd = { .fd = s->data_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 300);
        if (pr == 0) {
            if (!warned && !s->mitm && captured == 0 && bsdr_now_ms() - start > 12000) {
                BSDR_WARN("bsdr.micsniff", "no packets from Quest %s in 12s on %s — this host can't "
                          "see its traffic. Put the agent on the gateway / a SPAN (mirror) port, or use MITM.",
                          s->quest_ip, s->iface);
                warned = 1;
            }
            continue;
        }
        if (pr < 0) { if (errno == EINTR) continue; break; }
        ssize_t n = recv(s->data_fd, buf, sizeof buf, 0);
        if (n <= 0) break;                          /* helper gone */
        captured++;
        handle_ip(s, buf, (int)n);
        if (captured == 1)
            BSDR_INFO("bsdr.micsniff", "seeing Quest %s traffic; waiting for a room mic stream", s->quest_ip);
    }
    BSDR_INFO("bsdr.micsniff", "owner-mic sniffer stopped (%ld frames decoded)", s->decoded);
}

static int start_capture(struct bsdr_micsniff *s, const char *password) {
    if (s->remote_port > 0) {
        /* Router-companion mode: bind a UDP socket and receive captured IPv4 packets from the
         * router (bsdr_micrelay), which sees all the Quest's traffic as its gateway. No local
         * capture, no privileged helper — sniff_main reads this fd exactly like the helper's. */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { BSDR_ERROR("bsdr.micsniff", "remote socket: %s", strerror(errno)); return -1; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)s->remote_port);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            BSDR_ERROR("bsdr.micsniff", "remote bind udp/%d: %s", s->remote_port, strerror(errno));
            close(fd); return -1;
        }
        s->data_fd = fd; s->helper_pid = -1;
        BSDR_INFO("bsdr.micsniff", "owner mic via router companion: listening udp/%d for Quest %s packets",
                  s->remote_port, s->quest_ip);
        return 0;
    }
    return spawn_helper(s, password, &s->helper_pid, &s->data_fd);
}
static void stop_capture(struct bsdr_micsniff *s) {
    if (s->data_fd >= 0) { close(s->data_fd); s->data_fd = -1; }   /* helper: EOF → heal + exit */
    if (s->helper_pid > 0) {
        for (int i = 0; i < 30; i++) { int st; pid_t r = waitpid((pid_t)s->helper_pid, &st, WNOHANG);
            if (r == (pid_t)s->helper_pid || r < 0) { s->helper_pid = -1; break; } bsdr_sleep_ms(100); }
        if (s->helper_pid > 0) { kill((pid_t)s->helper_pid, SIGTERM); waitpid((pid_t)s->helper_pid, NULL, 0); s->helper_pid = -1; }
    }
}

/* ========================================================= WINDOWS (in-process) */
#elif defined(_WIN32)

int bsdr_micsniff_helper_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }  /* no helper on Windows */

/* Our interface's MAC + IPv4 + default gateway, matched to the Npcap device string (which embeds
 * the adapter GUID, e.g. \Device\NPF_{GUID}). Returns 0 with the MAC filled in. */
static int win_iface_info(const char *iface, uint8_t mac[6], uint32_t *ip_be, uint32_t *gw_be) {
    ULONG sz = 0;
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST;
    if (GetAdaptersAddresses(AF_INET, flags, NULL, NULL, &sz) != ERROR_BUFFER_OVERFLOW || !sz) return -1;
    IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(sz);
    if (!aa) return -1;
    int rc = -1;
    if (GetAdaptersAddresses(AF_INET, flags, NULL, aa, &sz) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *p = aa; p; p = p->Next) {
            if (!p->AdapterName || !strstr(iface, p->AdapterName)) continue;
            if (p->PhysicalAddressLength == 6) { memcpy(mac, p->PhysicalAddress, 6); rc = 0; }
            for (IP_ADAPTER_UNICAST_ADDRESS *u = p->FirstUnicastAddress; u; u = u->Next)
                if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                    *ip_be = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr; break; }
            if (!*gw_be)
                for (IP_ADAPTER_GATEWAY_ADDRESS *g = p->FirstGatewayAddress; g; g = g->Next)
                    if (g->Address.lpSockaddr && g->Address.lpSockaddr->sa_family == AF_INET) {
                        *gw_be = ((struct sockaddr_in *)g->Address.lpSockaddr)->sin_addr.s_addr; break; }
            break;
        }
    }
    free(aa);
    return rc;
}

/* Resolve a neighbour's MAC via the OS ARP layer (iphlpapi SendARP). src_be may be 0. */
static int win_resolve_mac(uint32_t ip_be, uint32_t src_be, uint8_t out[6]) {
    ULONG mac[2] = { 0, 0 }, mlen = 6;
    if (SendARP((IPAddr)ip_be, (IPAddr)src_be, mac, &mlen) != NO_ERROR || mlen < 6) return -1;
    memcpy(out, mac, 6);
    return 0;
}

/* Toggle system IP forwarding at runtime (no reboot). EnableRouter bumps a global refcount; keep the
 * handle + OVERLAPPED alive until UnenableRouter drops it back. */
static int win_forward_enable(struct bsdr_micsniff *s) {
    OVERLAPPED *ov = (OVERLAPPED *)calloc(1, sizeof *ov);
    if (!ov) return -1;
    ov->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    HANDLE h = NULL;
    DWORD r = EnableRouter(&h, ov);
    if (r != ERROR_IO_PENDING && r != NO_ERROR) {
        if (ov->hEvent) CloseHandle(ov->hEvent);
        free(ov);
        return -1;
    }
    s->w_router_handle = h; s->w_router_ovl = ov; s->w_router_on = 1;
    return 0;
}
static void win_forward_restore(struct bsdr_micsniff *s) {
    if (!s->w_router_on) return;
    DWORD count = 0;
    OVERLAPPED *ov = (OVERLAPPED *)s->w_router_ovl;
    if (ov) UnenableRouter(ov, &count);
    if (s->w_router_handle) CloseHandle((HANDLE)s->w_router_handle);
    if (ov) { if (ov->hEvent) CloseHandle(ov->hEvent); free(ov); }
    s->w_router_handle = s->w_router_ovl = NULL; s->w_router_on = 0;
}

static void sniff_main(void *arg) {
    struct bsdr_micsniff *s = (struct bsdr_micsniff *)arg;
    BSDR_INFO("bsdr.micsniff", "%s-sniffing Quest %s room mic (Npcap) -> %s",
              s->mitm ? "MITM" : "passive", s->quest_ip, OWNER_DESC);
    unsigned char buf[2048];
    long captured = 0; int warned = 0; uint64_t start = bsdr_now_ms();
    while (!s->stop) {
        if (s->mitm && s->w_have_mine) {            /* re-assert the poisoned entries ~1 Hz */
            uint64_t now = bsdr_now_ms();
            if (now - s->w_last_arp >= 1000) {
                if (!s->w_have_qmac) s->w_have_qmac = (win_resolve_mac(s->quest_be, s->w_our_ip, s->w_quest_mac) == 0);
                if (!s->w_have_gmac) s->w_have_gmac = (win_resolve_mac(s->w_gw_be,  s->w_our_ip, s->w_gw_mac)    == 0);
                uint8_t f[42];
                if (s->w_have_qmac) { build_arp(f, BSDR_ARP_REPLY, s->w_our_mac, s->w_gw_be, s->w_quest_mac, s->quest_be); mc_cap_inject(s->cap, f, 42); }
                if (s->w_have_gmac) { build_arp(f, BSDR_ARP_REPLY, s->w_our_mac, s->quest_be, s->w_gw_mac, s->w_gw_be); mc_cap_inject(s->cap, f, 42); }
                s->w_last_arp = now;
            }
        }
        int n = mc_cap_next(s->cap, buf, sizeof buf, 300);
        if (n < 0) break;
        if (n == 0) {
            if (!warned && !s->mitm && captured == 0 && bsdr_now_ms() - start > 12000) {
                BSDR_WARN("bsdr.micsniff", "no packets from Quest %s in 12s — this host can't see its "
                          "traffic (be the gateway or a mirror port, or use --sniff-mitm).", s->quest_ip);
                warned = 1;
            }
            continue;
        }
        captured++;
        handle_ip(s, buf, n);
    }
    if (s->mitm && s->w_have_mine && s->w_have_qmac && s->w_have_gmac)   /* heal both caches */
        for (int i = 0; i < 3; i++) {
            uint8_t f[42];
            build_arp(f, BSDR_ARP_REPLY, s->w_gw_mac, s->w_gw_be, s->w_quest_mac, s->quest_be); mc_cap_inject(s->cap, f, 42);
            build_arp(f, BSDR_ARP_REPLY, s->w_quest_mac, s->quest_be, s->w_gw_mac, s->w_gw_be); mc_cap_inject(s->cap, f, 42);
            bsdr_sleep_ms(150);
        }
    BSDR_INFO("bsdr.micsniff", "owner-mic sniffer stopped (%ld frames decoded)", s->decoded);
}

static int start_capture(struct bsdr_micsniff *s, const char *password) {
    (void)password;
    char err[256] = "";
    s->cap = mc_cap_open(s->iface, s->quest_ip, err, sizeof err);
    if (!s->cap) {
        BSDR_ERROR("bsdr.micsniff", "capture open failed: %s", err);
        BSDR_ERROR("bsdr.micsniff", "Windows needs Npcap installed (https://npcap.com) and the agent "
                   "run as Administrator.");
        return -1;
    }
    s->data_fd = -1;

    if (s->mitm) {                                  /* arm the in-process ARP poisoner */
        if (s->gw_ip[0]) s->w_gw_be = inet_addr(s->gw_ip);   /* explicit override wins */
        s->w_have_mine = (win_iface_info(s->iface, s->w_our_mac, &s->w_our_ip, &s->w_gw_be) == 0);
        if (!s->w_have_mine || !s->w_gw_be || s->w_gw_be == INADDR_NONE) {
            BSDR_ERROR("bsdr.micsniff", "MITM: can't determine our MAC/gateway on %s "
                       "(pass --sniff-iface as the Npcap device, --sniff-gw for the gateway) — passive only", s->iface);
            s->mitm = 0;
        } else if (win_forward_enable(s) != 0) {
            BSDR_ERROR("bsdr.micsniff", "MITM: EnableRouter failed (run as Administrator) — passive only");
            s->mitm = 0;
        } else {
            s->w_have_qmac = (win_resolve_mac(s->quest_be, s->w_our_ip, s->w_quest_mac) == 0);
            s->w_have_gmac = (win_resolve_mac(s->w_gw_be,  s->w_our_ip, s->w_gw_mac)    == 0);
            struct in_addr ga; ga.s_addr = s->w_gw_be;
            BSDR_INFO("bsdr.micsniff", "MITM armed on %s: gateway %s, forwarding on (qmac=%d gmac=%d)",
                      s->iface, inet_ntoa(ga), s->w_have_qmac, s->w_have_gmac);
            if (!s->w_have_qmac || !s->w_have_gmac)
                BSDR_WARN("bsdr.micsniff", "MITM degraded — a neighbour MAC didn't resolve; ARP will retry");
        }
    }
    return 0;
}
static void stop_capture(struct bsdr_micsniff *s) {
    win_forward_restore(s);
    if (s->cap) { mc_cap_close(s->cap); s->cap = NULL; }
}

#endif

/* ============================================================ public API (all) */

#if defined(__ANDROID__)
/* Android has no local capture; the router companion (bsdr_micrelay) forwards the Quest's owner-mic
 * packets over UDP. Receive + run the SAME decode + voice-changer path (handle_ip) as desktop. */
static void android_relay_main(void *arg) {
    struct bsdr_micsniff *s = (struct bsdr_micsniff *)arg;
    unsigned char buf[2048];
    while (!s->stop) {
        struct pollfd pfd = { .fd = s->data_fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 300) <= 0) continue;
        ssize_t n = recv(s->data_fd, buf, sizeof buf, 0);
        if (n <= 0) { if (n < 0) break; continue; }
        handle_ip(s, buf, (int)n);
    }
}
#endif

bsdr_micsniff *bsdr_micsniff_start(const bsdr_micsniff_cfg *cfg) {
    if (!cfg || !cfg->quest_ip || !cfg->quest_ip[0]) {
        BSDR_WARN("bsdr.micsniff", "no quest_ip given; owner-mic sniffer disabled");
        return NULL;
    }
    struct bsdr_micsniff *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->data_fd = -1; s->helper_pid = -1; s->sink_mod = s->src_mod = -1;
    s->mitm = cfg->mitm ? 1 : 0;
    s->remote_port = cfg->remote_port;
    if (s->remote_port > 0) s->mitm = 0;   /* the router companion captures; we only receive */
    snprintf(s->quest_ip, sizeof s->quest_ip, "%s", cfg->quest_ip);
    s->quest_be = inet_addr(cfg->quest_ip);
    if (s->quest_be == INADDR_NONE) { BSDR_ERROR("bsdr.micsniff", "bad quest_ip %s", cfg->quest_ip); free(s); return NULL; }
    if (cfg->iface && cfg->iface[0]) snprintf(s->iface, sizeof s->iface, "%s", cfg->iface);
    if (cfg->gateway_ip && cfg->gateway_ip[0]) snprintf(s->gw_ip, sizeof s->gw_ip, "%s", cfg->gateway_ip);
#if defined(__ANDROID__)
    /* Android: relay-only — no local capture, no virtual mic. Bind the relay UDP port, decode +
     * apply voice FX (handle_ip), feed the pcm tap. */
    if (s->remote_port <= 0) { BSDR_WARN("bsdr.micsniff", "Android owner mic needs the router companion — set the relay port"); free(s); return NULL; }
    { int err = 0; s->dec = opus_decoder_create(48000, 1, &err);
      if (!s->dec || err != OPUS_OK) { free(s); return NULL; } }
    { int fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd < 0) goto fail;
      int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      struct sockaddr_in aa; memset(&aa, 0, sizeof aa);
      aa.sin_family = AF_INET; aa.sin_addr.s_addr = htonl(INADDR_ANY); aa.sin_port = htons((uint16_t)s->remote_port);
      if (bind(fd, (struct sockaddr *)&aa, sizeof aa) != 0) { BSDR_ERROR("bsdr.micsniff", "relay: bind udp %d failed", s->remote_port); close(fd); goto fail; }
      s->data_fd = fd; }
    s->thr = bsdr_thread_start(android_relay_main, s);
    if (!s->thr) { BSDR_ERROR("bsdr.micsniff", "relay thread start failed"); goto fail; }
    BSDR_INFO("bsdr.micsniff", "Android owner mic: relay on udp %d (quest %s)", s->remote_port, s->quest_ip);
    return s;
#else
#if POSIX_HELPER
    if (s->remote_port <= 0) default_route(s->iface, sizeof s->iface, s->gw_ip, sizeof s->gw_ip);
#endif
    if (s->remote_port <= 0 && !s->iface[0]) {
        BSDR_ERROR("bsdr.micsniff", "no capture interface (pass --sniff-iface)"); free(s); return NULL; }

    if (!bsdr_virtual_mic_create(OWNER_SINK, OWNER_SOURCE, OWNER_DESC, &s->sink_mod, &s->src_mod)) goto fail;
    s->player = bsdr_audio_player_new(OWNER_SINK, 1);
    int err = 0; s->dec = opus_decoder_create(48000, 1, &err);
    if (!s->player || err != OPUS_OK || !s->dec) { BSDR_ERROR("bsdr.micsniff", "player/decoder init failed"); goto fail; }

    if (start_capture(s, cfg->password) != 0) goto fail;
    s->thr = bsdr_thread_start(sniff_main, s);
    if (!s->thr) { BSDR_ERROR("bsdr.micsniff", "sniffer thread start failed"); goto fail; }
    return s;
#endif
fail:
    bsdr_micsniff_stop(s);
    return NULL;
}

bool bsdr_micsniff_is_mitm(const bsdr_micsniff *s) { return s && s->mitm; }

void bsdr_micsniff_set_pcm_sink(bsdr_micsniff *s, bsdr_micsniff_pcm_cb cb, void *user) {
    if (!s) return;
    s->pcm_user = user;
    s->pcm_cb = cb;   /* set user before cb so the sniffer thread never sees a stale pair */
}

void bsdr_micsniff_set_voicefx(bsdr_micsniff *s, int gender, int robot, int echo, int whisper) {
    if (!s) return;
    if (gender < -100) gender = -100; else if (gender > 100) gender = 100;
    s->fxp.gender = gender; s->fxp.robot = robot; s->fxp.echo = echo; s->fxp.whisper = whisper;
}

void bsdr_micsniff_stop(bsdr_micsniff *s) {
    if (!s) return;
    s->stop = 1;
    if (s->thr) { bsdr_thread_join(s->thr); s->thr = NULL; }
#if defined(__ANDROID__)
    if (s->data_fd >= 0) { close(s->data_fd); s->data_fd = -1; }
#else
    stop_capture(s);
    if (s->player) { bsdr_audio_player_free(s->player); s->player = NULL; }
#endif
    if (s->dec) { opus_decoder_destroy(s->dec); s->dec = NULL; }
    if (s->fx) { bsdr_voicefx_free(s->fx); s->fx = NULL; }
#if !defined(__ANDROID__)
    if (s->sink_mod >= 0 || s->src_mod >= 0) bsdr_virtual_mic_destroy(s->sink_mod, s->src_mod);
#endif
    free(s);
}

#else  /* no audio backend: the owner mic is unavailable */

bsdr_micsniff *bsdr_micsniff_start(const bsdr_micsniff_cfg *cfg) { (void)cfg; return NULL; }
void bsdr_micsniff_stop(bsdr_micsniff *s) { (void)s; }
bool bsdr_micsniff_is_mitm(const bsdr_micsniff *s) { (void)s; return false; }
void bsdr_micsniff_set_pcm_sink(bsdr_micsniff *s, bsdr_micsniff_pcm_cb cb, void *user) { (void)s; (void)cb; (void)user; }
void bsdr_micsniff_set_voicefx(bsdr_micsniff *s, int g, int r, int e, int w) { (void)s; (void)g; (void)r; (void)e; (void)w; }
int bsdr_micsniff_helper_main(int argc, char **argv) { (void)argc; (void)argv; return 1; }

#endif
