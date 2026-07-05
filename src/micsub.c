/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* micsub.c — owner-mic substitution via NFQUEUE payload rewrite. See micsub.h.
 * Built only with BSDR_HAVE_NFQUEUE (Linux + libnetfilter_queue); a no-op stub otherwise. */
#include "bsdr/micsub.h"
#include "bsdr/log.h"

#include <stddef.h>

#if defined(BSDR_HAVE_NFQUEUE)

#include "bsdr/voicefx.h"
#include "bsdr/platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <opus/opus.h>

#include <libnetfilter_queue/libnetfilter_queue.h>
#include <linux/netfilter.h>

struct bsdr_micsub {
    uint32_t quest_be;
    int      queue_num;
    struct nfq_handle   *h;
    struct nfq_q_handle *qh;
    int      fd;
    OpusDecoder *dec;
    OpusEncoder *enc;
    bsdr_voicefx *fx;
    bsdr_voicefx_params fxp;
    bsdr_thread *thr;
    volatile int stop;
    int      rule_added;
    char     quest_ip[64];
    /* owner-mic flow lock (mono Opus, like micsniff) — rewrite only this flow */
    int      have_flow;
    uint32_t flow_dst_be; uint16_t flow_dport; uint32_t flow_ssrc;
    long     rewritten;
};

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
 * Returns the new packet length (may differ), or 0 to leave the packet unchanged. */
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
        struct in_addr a; a.s_addr = dst_be;
        BSDR_INFO("bsdr.micsub", "locked owner-mic flow -> %s:%u ssrc=%u (rewriting)", inet_ntoa(a), dport, ssrc);
    }
    if (dst_be != s->flow_dst_be || dport != s->flow_dport || ssrc != s->flow_ssrc) return 0;

    int16_t pcm[5760];
    int fr = opus_decode(s->dec, pl, olen, pcm, 5760, 0);
    if (fr <= 0) return 0;

    /* voice change (lazy engine) */
    if (s->fxp.gender || s->fxp.robot || s->fxp.echo || s->fxp.whisper) {
        if (!s->fx) s->fx = bsdr_voicefx_new(48000);
        if (s->fx) { bsdr_voicefx_set_params(s->fx, &s->fxp); bsdr_voicefx_process(s->fx, pcm, fr); }
    }

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

bsdr_micsub *bsdr_micsub_start(const char *quest_ip, int queue_num) {
    if (!quest_ip || !quest_ip[0]) return NULL;
    struct bsdr_micsub *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->quest_ip, sizeof s->quest_ip, "%s", quest_ip);
    s->quest_be = (uint32_t)inet_addr(quest_ip);
    s->queue_num = queue_num;
    if (s->quest_be == INADDR_NONE) { free(s); return NULL; }

    int err = 0;
    s->dec = opus_decoder_create(48000, 1, &err);
    s->enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!s->dec || !s->enc) { BSDR_ERROR("bsdr.micsub", "opus init failed"); goto fail; }
    opus_encoder_ctl(s->enc, OPUS_SET_BITRATE(24000));

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
    s->thr = bsdr_thread_start(micsub_main, s);
    if (!s->thr) goto fail;
    BSDR_INFO("bsdr.micsub", "owner-mic substitution active (queue %d, quest %s) — room hears the changed voice",
              queue_num, quest_ip);
    return s;
fail:
    bsdr_micsub_stop(s);
    return NULL;
}

void bsdr_micsub_set_voicefx(bsdr_micsub *s, int gender, int robot, int echo, int whisper) {
    if (!s) return;
    if (gender < -100) gender = -100; else if (gender > 100) gender = 100;
    s->fxp.gender = gender; s->fxp.robot = robot; s->fxp.echo = echo; s->fxp.whisper = whisper;
}

void bsdr_micsub_stop(bsdr_micsub *s) {
    if (!s) return;
    s->stop = 1;
    if (s->thr) { bsdr_thread_join(s->thr); s->thr = NULL; }
    if (s->rule_added) iptables_rule(s->quest_ip, s->queue_num, 0);
    if (s->qh) nfq_destroy_queue(s->qh);
    if (s->h) nfq_close(s->h);
    if (s->fx) bsdr_voicefx_free(s->fx);
    if (s->dec) opus_decoder_destroy(s->dec);
    if (s->enc) opus_encoder_destroy(s->enc);
    free(s);
}

#else  /* no NFQUEUE (non-Linux, or libnetfilter_queue absent) */

bsdr_micsub *bsdr_micsub_start(const char *quest_ip, int queue_num) {
    (void)quest_ip; (void)queue_num;
    BSDR_WARN("bsdr.micsub", "owner-mic substitution needs Linux + libnetfilter_queue (build with BSDR_HAVE_NFQUEUE)");
    return NULL;
}
void bsdr_micsub_set_voicefx(bsdr_micsub *s, int g, int r, int e, int w) { (void)s;(void)g;(void)r;(void)e;(void)w; }
void bsdr_micsub_stop(bsdr_micsub *s) { (void)s; }

#endif
