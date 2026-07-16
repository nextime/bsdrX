/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* micsub.c — owner-mic SUBSTITUTION into the cloud room. See micsub.h.
 *
 * Rewrites the Quest->cloud owner-mic Opus RTP in flight with the voice-changed audio, so the ROOM
 * hears the changed voice (the local voice CHANGER only affects our own BSDR_QuestMic copy). Only
 * works while we are the MITM, so the flow transits us.
 *
 * The rewrite() core (Opus decode -> voicefx -> re-encode, keeping the RTP header/ssrc/8-byte cloud
 * trailer, fixing IP+UDP checksums) is platform-neutral; only the packet-intercept primitive differs:
 *   - Linux   : NFQUEUE (libnetfilter_queue) on the FORWARD chain     -> BSDR_HAVE_NFQUEUE
 *   - Windows : WinDivert at the NETWORK_FORWARD layer (needs Admin)  -> BSDR_HAVE_WINDIVERT
 * Neither present (e.g. Android, or a build without the lib) -> a no-op stub.
 */
#include "bsdr/micsub.h"
#include "bsdr/log.h"

#include <stddef.h>

#if defined(BSDR_HAVE_NFQUEUE) || defined(BSDR_HAVE_WINDIVERT)
#define BSDR_MICSUB_ACTIVE 1
#endif

#if defined(BSDR_MICSUB_ACTIVE)

#include "bsdr/voicefx.h"
#include "bsdr/mediafx.h"
#include "bsdr/platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <opus/opus.h>

#if defined(BSDR_HAVE_NFQUEUE)
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <poll.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <linux/netfilter.h>
#elif defined(BSDR_HAVE_WINDIVERT)
#include <windows.h>
#include <windivert.h>
#endif

struct bsdr_micsub {
    uint32_t quest_be;                 /* Quest IPv4, wire byte order (matches a memcpy from the packet) */
    char     quest_ip[64];
    OpusDecoder *dec;
    OpusEncoder *enc;
    /* Legacy voice-fx config, still populated by the web UI via the setters below but no longer consumed
     * (the voice-changer PLUGIN owns the effect). Kept so the setters + callers keep compiling. */
    bsdr_voicefx_params fxp;
    struct { int on, tier, voice_sr, key; char content[1024], rmvpe[1024], voice[1024]; } ai;
    int ai_dirty;
    bsdr_thread *thr;
    volatile int stop;
    /* owner-mic flow lock (mono Opus, like micsniff) — rewrite only this flow */
    int      have_flow;
    uint32_t flow_dst_be; uint16_t flow_dport; uint32_t flow_ssrc;
    long     rewritten;
#if defined(BSDR_HAVE_NFQUEUE)
    int      queue_num;
    struct nfq_handle   *h;
    struct nfq_q_handle *qh;
    int      fd;
    int      rule_added;
#elif defined(BSDR_HAVE_WINDIVERT)
    HANDLE   wd;
#endif
};

/* Parse "a.b.c.d" into a wire-order uint32 (bytes [a,b,c,d] in memory), matching a memcpy of the IP
 * source field. Avoids inet_addr so the shared core needs no winsock/arpa headers. Returns 1 on ok. */
static int parse_ipv4_be(const char *s, uint32_t *out) {
    unsigned a, b, c, d;
    if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    uint8_t *p = (uint8_t *)out;
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)c; p[3] = (uint8_t)d;
    return 1;
}

/* ---- checksums ---- */
static uint16_t csum16(const uint8_t *p, int len, uint32_t sum) {
    while (len > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static void ip_checksum(uint8_t *ip, int ihl) {
    ip[10] = ip[11] = 0;
    uint16_t c = csum16(ip, ihl, 0);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)(c & 0xff);
}
static void udp_checksum(uint8_t *ip, int ihl, uint8_t *udp, int ulen) {
    udp[6] = udp[7] = 0;
    /* pseudo-header: src(4) dst(4) zero(1) proto(1)=17 udp_len(2) */
    uint32_t sum = 0;
    sum += (ip[12] << 8) | ip[13]; sum += (ip[14] << 8) | ip[15];   /* src */
    sum += (ip[16] << 8) | ip[17]; sum += (ip[18] << 8) | ip[19];   /* dst */
    sum += 17;
    sum += (uint16_t)ulen;
    uint16_t c = csum16(udp, ulen, sum);
    if (c == 0) c = 0xffff;
    udp[6] = (uint8_t)(c >> 8); udp[7] = (uint8_t)(c & 0xff);
}

/* Rewrite the owner-mic Opus payload in `pkt` (a full IPv4 datagram) with the voice-changed audio.
 * Returns the new packet length (may differ), or 0 to leave the packet unchanged. Platform-neutral. */
static int rewrite(struct bsdr_micsub *s, uint8_t *pkt, int len, uint8_t *out, int outcap) {
    if (len < 20) return 0;
    int ver = pkt[0] >> 4; if (ver != 4) return 0;
    int ihl = (pkt[0] & 0x0f) * 4; if (ihl < 20 || ihl > len) return 0;
    if (pkt[9] != 17) return 0;                       /* UDP */
    uint32_t src_be; memcpy(&src_be, pkt + 12, 4);
    if (src_be != s->quest_be) return 0;              /* only the Quest's uplink */
    uint32_t dst_be; memcpy(&dst_be, pkt + 16, 4);
    uint8_t *udp = pkt + ihl;
    if (ihl + 8 > len) return 0;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    int ulen = (udp[4] << 8) | udp[5];
    if (ulen < 8 || ihl + ulen > len) ulen = len - ihl;
    uint8_t *rtp = udp + 8;
    int rlen = ulen - 8;
    if (rlen < 12) return 0;
    if ((rtp[0] >> 6) != 2) return 0;                 /* RTP v2 */
    int hlen = 12 + 4 * (rtp[0] & 0x0f);
    if (rtp[0] & 0x10) { if (rlen < hlen + 4) return 0; int extw = (rtp[hlen + 2] << 8) | rtp[hlen + 3]; hlen += 4 + 4 * extw; }
    if (rlen <= hlen + 8) return 0;
    uint32_t ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) | ((uint32_t)rtp[10] << 8) | rtp[11];
    uint8_t *pl = rtp + hlen;
    int plen = rlen - hlen;
    int olen = plen - 8;                              /* opus bytes (last 8 = trailer) */
    uint8_t *tr = pl + olen;

    if (!s->have_flow) {
        if (pl[0] & 0x04) return 0;                   /* stereo TOC -> not the mono owner mic */
        int16_t probe[5760];
        if (opus_decode(s->dec, pl, olen, probe, 5760, 0) <= 0) return 0;
        s->flow_dst_be = dst_be; s->flow_dport = dport; s->flow_ssrc = ssrc; s->have_flow = 1;
        const uint8_t *db = (const uint8_t *)&dst_be;
        BSDR_INFO("bsdr.micsub", "locked owner-mic flow -> %u.%u.%u.%u:%u ssrc=%u (rewriting)",
                  db[0], db[1], db[2], db[3], dport, ssrc);
    }
    if (dst_be != s->flow_dst_be || dport != s->flow_dport || ssrc != s->flow_ssrc) return 0;

    int16_t pcm[5760];
    int fr = opus_decode(s->dec, pl, olen, pcm, 5760, 0);
    if (fr <= 0) return 0;

    /* Voice change is delivered by the voice-changer PLUGIN now (via the media-fx hook): route the mic
     * PCM through it if one is loaded; otherwise the owner's voice is unchanged. The legacy fxp/ai setters
     * still exist (fed by the web UI) but are no longer consumed here. */
    bsdr_mediafx_apply_audio(pcm, fr, 48000, 1);

    uint8_t neu[1500];
    int nolen = opus_encode(s->enc, pcm, fr, neu, sizeof neu);
    if (nolen <= 0) return 0;

    /* rebuild: [ip hdr][udp hdr][rtp hdr][new opus][8B trailer] */
    int new_rlen = hlen + nolen + 8;
    int new_ulen = 8 + new_rlen;
    int new_iplen = ihl + new_ulen;
    if (new_iplen > outcap) return 0;
    memcpy(out, pkt, ihl + 8 + hlen);                 /* ip + udp + rtp headers verbatim */
    memcpy(out + ihl + 8 + hlen, neu, nolen);         /* new opus */
    memcpy(out + ihl + 8 + hlen + nolen, tr, 8);      /* trailer (ssrc + frame_id) verbatim */
    /* fix lengths */
    out[2] = (uint8_t)(new_iplen >> 8); out[3] = (uint8_t)(new_iplen & 0xff);   /* IP total length */
    out[ihl + 4] = (uint8_t)(new_ulen >> 8); out[ihl + 5] = (uint8_t)(new_ulen & 0xff);  /* UDP length */
    ip_checksum(out, ihl);
    udp_checksum(out, ihl, out + ihl, new_ulen);
    s->rewritten++;
    return new_iplen;
}

/* ============================================================ NFQUEUE backend (Linux) */
#if defined(BSDR_HAVE_NFQUEUE)

static int nfq_cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa, void *data) {
    (void)nfmsg;
    struct bsdr_micsub *s = (struct bsdr_micsub *)data;
    struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfa);
    uint32_t id = ph ? ntohl(ph->packet_id) : 0;
    unsigned char *pkt = NULL;
    int len = nfq_get_payload(nfa, &pkt);
    if (len < 0 || !pkt) return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
    uint8_t out[2048];
    int n = rewrite(s, pkt, len, out, sizeof out);
    if (n > 0) return nfq_set_verdict(qh, id, NF_ACCEPT, n, out);   /* modified */
    return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);             /* verbatim */
}

/* ---- iptables rule (fail-open with --queue-bypass so a handler crash never blocks the Quest) ---- */
static int iptables_rule(const char *quest_ip, int qnum, int add) {
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "iptables -%c FORWARD -s %s -p udp -j NFQUEUE --queue-num %d --queue-bypass 2>/dev/null",
             add ? 'I' : 'D', quest_ip, qnum);
    return system(cmd);
}

static void micsub_main(void *arg) {
    struct bsdr_micsub *s = (struct bsdr_micsub *)arg;
    char buf[4096];
    while (!s->stop) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 300);
        if (pr <= 0) continue;
        int r = recv(s->fd, buf, sizeof buf, 0);
        if (r >= 0) nfq_handle_packet(s->h, buf, r);
        else if (errno != ENOBUFS) break;
    }
}

/* ============================================================ WinDivert backend (Windows) */
#elif defined(BSDR_HAVE_WINDIVERT)

static void micsub_main(void *arg) {
    struct bsdr_micsub *s = (struct bsdr_micsub *)arg;
    UINT8 *buf = (UINT8 *)malloc(65536);
    UINT8 *out = (UINT8 *)malloc(65536);
    if (!buf || !out) { free(buf); free(out); return; }
    while (!s->stop) {
        UINT recvLen = 0;
        WINDIVERT_ADDRESS addr;
        if (!WinDivertRecv(s->wd, buf, 65536, &recvLen, &addr)) {
            if (s->stop) break;
            DWORD e = GetLastError();
            if (e == ERROR_NO_DATA || e == ERROR_INVALID_HANDLE || e == ERROR_OPERATION_ABORTED) break;
            continue;                                  /* transient (e.g. buffer too small) — keep going */
        }
        int n = rewrite(s, buf, (int)recvLen, out, 65536);
        if (n > 0) {
            WINDIVERT_ADDRESS a2 = addr;
            a2.IPChecksum = 0; a2.UDPChecksum = 0;     /* force the driver to recompute */
            WinDivertHelperCalcChecksums(out, (UINT)n, &a2, 0);
            UINT sent = 0;
            WinDivertSend(s->wd, out, (UINT)n, &sent, &a2);   /* modified */
        } else {
            UINT sent = 0;
            WinDivertSend(s->wd, buf, recvLen, &sent, &addr);  /* verbatim — MUST re-inject */
        }
    }
    free(buf); free(out);
}

#endif  /* backend */

/* ============================================================ shared start/stop */
bsdr_micsub *bsdr_micsub_start(const char *quest_ip, int queue_num) {
    if (!quest_ip || !quest_ip[0]) return NULL;
    struct bsdr_micsub *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->quest_ip, sizeof s->quest_ip, "%s", quest_ip);
    if (!parse_ipv4_be(quest_ip, &s->quest_be)) { free(s); return NULL; }
#if defined(BSDR_HAVE_WINDIVERT)
    s->wd = INVALID_HANDLE_VALUE;
    (void)queue_num;
#endif

    int err = 0;
    s->dec = opus_decoder_create(48000, 1, &err);
    s->enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!s->dec || !s->enc) { BSDR_ERROR("bsdr.micsub", "opus init failed"); goto fail; }
    opus_encoder_ctl(s->enc, OPUS_SET_BITRATE(24000));

#if defined(BSDR_HAVE_NFQUEUE)
    s->queue_num = queue_num;
    s->h = nfq_open();
    if (!s->h) { BSDR_ERROR("bsdr.micsub", "nfq_open failed (need CAP_NET_ADMIN/root)"); goto fail; }
    nfq_unbind_pf(s->h, AF_INET);
    if (nfq_bind_pf(s->h, AF_INET) < 0) { BSDR_ERROR("bsdr.micsub", "nfq_bind_pf failed"); goto fail; }
    s->qh = nfq_create_queue(s->h, (uint16_t)queue_num, &nfq_cb, s);
    if (!s->qh) { BSDR_ERROR("bsdr.micsub", "nfq_create_queue %d failed", queue_num); goto fail; }
    if (nfq_set_mode(s->qh, NFQNL_COPY_PACKET, 0xffff) < 0) { BSDR_ERROR("bsdr.micsub", "nfq_set_mode failed"); goto fail; }
    s->fd = nfq_fd(s->h);
    if (iptables_rule(quest_ip, queue_num, 1) != 0) {
        BSDR_ERROR("bsdr.micsub", "iptables NFQUEUE rule failed (need root; is iptables present?)");
        goto fail;
    }
    s->rule_added = 1;

#elif defined(BSDR_HAVE_WINDIVERT)
    /* Intercept the forwarded (routed) UDP from the Quest. The in-process MITM enables routing, so the
     * Quest->cloud voice transits us and appears at the NETWORK_FORWARD layer. */
    char filter[160];
    snprintf(filter, sizeof filter, "ip and udp and ip.SrcAddr == %s", quest_ip);
    s->wd = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK_FORWARD, 0, 0);
    if (s->wd == INVALID_HANDLE_VALUE) {
        BSDR_ERROR("bsdr.micsub", "WinDivertOpen failed (err %lu) — run as Administrator and ensure the "
                   "WinDivert driver (WinDivert.dll + WinDivert64.sys) is next to the exe",
                   (unsigned long)GetLastError());
        goto fail;
    }
#endif

    s->thr = bsdr_thread_start(micsub_main, s);
    if (!s->thr) goto fail;
    BSDR_INFO("bsdr.micsub", "owner-mic substitution active (quest %s) — room hears the changed voice", quest_ip);
    return s;
fail:
    bsdr_micsub_stop(s);
    return NULL;
}

void bsdr_micsub_set_voicefx(bsdr_micsub *s, int gender, int formant, int volume,
                             int robot, int echo, int whisper) {
    if (!s) return;
    if (gender < -100) gender = -100; else if (gender > 100) gender = 100;
    s->fxp.gender = gender; s->fxp.formant = formant; s->fxp.volume = volume;
    s->fxp.robot = robot; s->fxp.echo = echo; s->fxp.whisper = whisper;
}

void bsdr_micsub_set_voiceai(bsdr_micsub *s, int on, int tier, const char *content,
                             const char *rmvpe, const char *voice, int voice_sr, int key) {
    if (!s) return;
    content = content ? content : ""; rmvpe = rmvpe ? rmvpe : ""; voice = voice ? voice : "";
    int changed = (s->ai.on != on) || (s->ai.tier != tier) || (s->ai.voice_sr != voice_sr) ||
                  (s->ai.key != key) || strcmp(s->ai.content, content) || strcmp(s->ai.rmvpe, rmvpe) ||
                  strcmp(s->ai.voice, voice);
    if (!changed) return;
    s->ai.on = on; s->ai.tier = tier; s->ai.voice_sr = voice_sr; s->ai.key = key;
    snprintf(s->ai.content, sizeof s->ai.content, "%s", content);
    snprintf(s->ai.rmvpe,   sizeof s->ai.rmvpe,   "%s", rmvpe);
    snprintf(s->ai.voice,   sizeof s->ai.voice,   "%s", voice);
    s->ai_dirty = 1;
}

void bsdr_micsub_stop(bsdr_micsub *s) {
    if (!s) return;
    s->stop = 1;
#if defined(BSDR_HAVE_WINDIVERT)
    if (s->wd && s->wd != INVALID_HANDLE_VALUE) WinDivertShutdown(s->wd, WINDIVERT_SHUTDOWN_BOTH);
#endif
    if (s->thr) { bsdr_thread_join(s->thr); s->thr = NULL; }
#if defined(BSDR_HAVE_NFQUEUE)
    if (s->rule_added) iptables_rule(s->quest_ip, s->queue_num, 0);
    if (s->qh) nfq_destroy_queue(s->qh);
    if (s->h) nfq_close(s->h);
#elif defined(BSDR_HAVE_WINDIVERT)
    if (s->wd && s->wd != INVALID_HANDLE_VALUE) { WinDivertClose(s->wd); s->wd = INVALID_HANDLE_VALUE; }
#endif
    if (s->dec) opus_decoder_destroy(s->dec);
    if (s->enc) opus_encoder_destroy(s->enc);
    free(s);
}

#else  /* no intercept backend (e.g. Android, or a build without libnetfilter_queue / WinDivert) */

bsdr_micsub *bsdr_micsub_start(const char *quest_ip, int queue_num) {
    (void)quest_ip; (void)queue_num;
    BSDR_WARN("bsdr.micsub", "owner-mic substitution needs Linux+libnetfilter_queue or Windows+WinDivert");
    return NULL;
}
void bsdr_micsub_set_voicefx(bsdr_micsub *s, int g, int fm, int vo, int r, int e, int w) { (void)s;(void)g;(void)fm;(void)vo;(void)r;(void)e;(void)w; }
void bsdr_micsub_set_voiceai(bsdr_micsub *s, int on, int t, const char *c, const char *r, const char *v, int sr, int k) { (void)s;(void)on;(void)t;(void)c;(void)r;(void)v;(void)sr;(void)k; }
void bsdr_micsub_stop(bsdr_micsub *s) { (void)s; }

#endif
