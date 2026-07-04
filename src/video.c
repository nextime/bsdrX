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
/* H.264 RTP (RFC 6184) + SRTP (libsrtp2) sender. BSDR_ENABLE_VIDEO. */
#include "bsdr/video.h"
#include "bsdr/protocol.h"
#include "bsdr/log.h"

#include <srtp2/srtp.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define RTP_HDR_LEN     12
#define MAX_RTP_PAYLOAD 1100      /* leave room under IP/UDP/DTLS/SRTP */
#define RTP_VERSION     0x80      /* V=2 */

struct bsdr_video_sender {
    bsdr_udp *udp;
    srtp_t srtp;
    bool plain;          /* BSDR_NO_SRTP: send unencrypted RTP (experiment) */
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t ts_offset;  /* random base added to caller ts (RFC 3550: don't start at 0) */
    uint32_t cloud_ts;   /* Bigscreen cloud path: ts counter, steps +1500 per packet */
    uint32_t cloud_frame; /* cloud trailer: per-NAL counter (frame field of the 16B trailer) */
    uint64_t cloud_prev_ms; /* cloud trailer: last AU wall-clock, for the ts_delta field */
    uint32_t packets;    /* RTP packets sent (for RTCP SR) */
    uint32_t octets;     /* RTP payload bytes sent (for RTCP SR) */
    uint32_t last_ts;    /* last RTP timestamp on the wire (ts + ts_offset) */
    uint8_t pkt[RTP_HDR_LEN + MAX_RTP_PAYLOAD + SRTP_MAX_TRAILER_LEN + 16];
};

static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v & 0xff;
}

static bool srtp_policy_for(srtp_policy_t *pol, const bsdr_srtp_keys *keys) {
    memset(pol, 0, sizeof(*pol));
    if (keys->profile == BSDR_SRTP_AES128_CM_SHA1_80) {
        srtp_crypto_policy_set_rtp_default(&pol->rtp);
        srtp_crypto_policy_set_rtcp_default(&pol->rtcp);
    } else if (keys->profile == BSDR_SRTP_AEAD_AES_128_GCM) {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol->rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol->rtcp);
    } else {
        return false;
    }
    return true;
}

bsdr_video_sender *bsdr_video_sender_new(bsdr_udp *udp,
                                         const bsdr_srtp_keys *keys,
                                         uint8_t payload_type, uint32_t ssrc) {
    bsdr_video_sender *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->udp = udp;
    v->payload_type = payload_type ? payload_type : BSDR_VIDEO_PT_DEFAULT;
    v->ssrc = ssrc ? ssrc : 0x42425344;   /* "BBSD" */

    /* NULL keys => plain RTP (no SRTP). The Bigscreen cloud media path is plain
     * RTP to the Mediasoup relay (jrtplib, no DTLS-SRTP — confirmed in BigSoup.dll).
     * Only init libsrtp when we actually use it — otherwise a libsrtp init failure would
     * (wrongly) null the sender and silently kill plain-RTP media. */
    v->plain = (keys == NULL) || getenv("BSDR_NO_SRTP") != NULL;
    if (!v->plain) {
        if (!bsdr_srtp_global_init()) { free(v); return NULL; }
        srtp_policy_t pol;
        if (!srtp_policy_for(&pol, keys)) {
            BSDR_ERROR("bsdr.video", "unsupported SRTP profile"); free(v); return NULL;
        }
        pol.ssrc.type = ssrc_specific;
        pol.ssrc.value = v->ssrc;
        pol.key = (unsigned char *)keys->send_master;
        pol.next = NULL;
        if (srtp_create(&v->srtp, &pol) != srtp_err_status_ok) {
            BSDR_ERROR("bsdr.video", "srtp_create failed"); free(v); return NULL;
        }
    }
    /* RFC 3550: start seq + timestamp at random values (the official host does; mediasoup
     * can mishandle a producer whose RTP starts at 0). Cheap entropy from the clock. */
    struct timespec rt; clock_gettime(CLOCK_MONOTONIC, &rt);
    uint32_t r = (uint32_t)(rt.tv_nsec ^ (rt.tv_sec << 17) ^ ((uintptr_t)v >> 4));
    v->seq = (uint16_t)(r & 0xffff);
    v->ts_offset = r * 2654435761u;
    v->cloud_ts = v->ts_offset;
    BSDR_INFO("bsdr.video", "%s video sender ready (pt=%u ssrc=%08x seq0=%u)",
              v->plain ? "PLAIN-RTP" : "SRTP", v->payload_type, v->ssrc, v->seq);
    return v;
}

/* Build RTP header + payload into v->pkt, srtp-protect, send. */
static int send_rtp(bsdr_video_sender *v, const uint8_t *payload, size_t plen,
                    uint32_t ts, int marker) {
    uint8_t *p = v->pkt;
    p[0] = RTP_VERSION;
    p[1] = (uint8_t)((marker ? 0x80 : 0) | (v->payload_type & 0x7f));
    wr16(p + 2, v->seq++);
    v->last_ts = ts + v->ts_offset;
    wr32(p + 4, v->last_ts);
    wr32(p + 8, v->ssrc);
    memcpy(p + RTP_HDR_LEN, payload, plen);
    v->packets++;
    v->octets += (uint32_t)plen;

    int len = (int)(RTP_HDR_LEN + plen);
    if (!v->plain && srtp_protect(v->srtp, p, &len) != srtp_err_status_ok) {
        BSDR_ERROR("bsdr.video", "srtp_protect failed");
        return -1;
    }
    return bsdr_udp_send(v->udp, p, (size_t)len) < 0 ? -1 : 0;
}

/* Bigscreen CLOUD H.264 packetization. Reversed from the official host's relay stream
 * (full.pcapng :27769): the cloud wire format is the SAME as the LAN BigSoup format MINUS the
 * XOR-0x14 cipher, wrapped in plain RTP. Each H.264 NAL is split into <=1372-byte chunks and
 * every RTP packet's payload is [NAL chunk][16-byte plaintext trailer]:
 *     trailer = [u32 sessid LE][u16 W LE][u16 H LE][u64 comp LE]
 *     comp    = frame<<40 | frag<<24 | total<<8 | ts_delta
 * W/H are macroblock-aligned (1080 -> 1088). The Quest depacketizer reassembles by the trailer's
 * (frame,frag,total) — NOT by packet size — so a fragment WITHOUT the trailer makes it read the
 * last 16 NAL bytes as garbage frame/frag indices: reassembly never completes, the hardware
 * decoder stalls, and the BigFrame queue OOMs (~26s). RTP ts steps +1500 per packet, marker 0,
 * SSRC=djb2(userSessionId) (== the trailer sessid). Each NAL = one frame_id (SPS/PPS/IDR each
 * distinct), sent once — the consumer bootstraps a keyframe from a consecutive 7/8/5 frame_id run. */
#define CLOUD_FRAG_NAL 1372       /* NAL bytes per fragment (= LAN 0x55c); +16 trailer = 1388 payload */
#define CLOUD_TRAILER  16
static const uint8_t *next_start(const uint8_t *p, const uint8_t *end, int *sclen);
static int nal_is_keepable(uint8_t nal_hdr);
static uint64_t mono_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint16_t align16(int v) { return (uint16_t)(((v + 15) / 16) * 16); }

/* Cloud comedia keepalive/latch: a VALID RTP packet with a 1-byte payload, matching the
 * official host (which sends these before + during the video so the Mediasoup PlainTransport
 * latches our source tuple). A bare junk byte isn't valid RTP and won't latch. */
int bsdr_video_send_keepalive(bsdr_video_sender *v) {
    if (!v) return -1;
    uint8_t pkt[RTP_HDR_LEN + 1];
    pkt[0] = RTP_VERSION;
    pkt[1] = (uint8_t)(v->payload_type & 0x7f);   /* marker 0 */
    wr16(pkt + 2, v->seq++);
    v->cloud_ts += 1500;
    v->last_ts = v->cloud_ts;
    wr32(pkt + 4, v->cloud_ts);
    wr32(pkt + 8, v->ssrc);
    pkt[RTP_HDR_LEN] = 0x00;
    return bsdr_udp_send(v->udp, pkt, RTP_HDR_LEN + 1) < 0 ? -1 : 0;
}

static int cloud_send_frag(bsdr_video_sender *v, const uint8_t *chunk, size_t clen,
                           uint16_t w, uint16_t h, uint32_t frame,
                           uint16_t fi, uint16_t total, uint8_t ts_delta) {
    uint8_t pkt[RTP_HDR_LEN + CLOUD_FRAG_NAL + CLOUD_TRAILER];
    pkt[0] = RTP_VERSION;
    pkt[1] = (uint8_t)(v->payload_type & 0x7f);   /* marker always 0 */
    wr16(pkt + 2, v->seq++);
    v->cloud_ts += 1500;
    v->last_ts = v->cloud_ts;
    wr32(pkt + 4, v->cloud_ts);
    wr32(pkt + 8, v->ssrc);
    uint8_t *pay = pkt + RTP_HDR_LEN;
    memcpy(pay, chunk, clen);
    /* Match the official client EXACTLY: it forces nal_ref_idc=1 on every NAL header
     * (0x67->0x27 SPS, 0x68->0x28 PPS, 0x65->0x25 IDR, 0x61->0x21 P), while our encoders
     * emit NRI=3. Only fragment 0 carries the NAL header byte; continuations are raw. The
     * type (low 5 bits) is preserved, so this stays valid H.264. */
    if (fi == 0 && clen > 0) pay[0] = (uint8_t)((pay[0] & 0x1f) | 0x20);
    uint8_t *tr = pay + clen;                      /* 16-byte trailer (plaintext) */
    uint32_t sessid = v->ssrc;                     /* cloud trailer sessid == djb2 SSRC */
    tr[0]=sessid; tr[1]=sessid>>8; tr[2]=sessid>>16; tr[3]=sessid>>24;
    tr[4]=w; tr[5]=w>>8; tr[6]=h; tr[7]=h>>8;
    uint64_t comp = ((((uint64_t)frame<<16 | fi)<<16 | total)<<8) | ts_delta;
    for (int b=0;b<8;b++) tr[8+b] = (uint8_t)(comp>>(8*b));
    size_t plen = clen + CLOUD_TRAILER;
    v->packets++; v->octets += (uint32_t)plen;
    return bsdr_udp_send(v->udp, pkt, (size_t)(RTP_HDR_LEN + plen)) < 0 ? -1 : 0;
}

int bsdr_video_send_au_cloud(bsdr_video_sender *v, const uint8_t *annexb, size_t len,
                             int src_w, int src_h) {
    /* debug: BSDR_DUMP_CLOUD=<file> writes the exact Annex-B access units we relay, so the cloud
     * H.264 + fragmentation can be verified offline (ffmpeg-decode + NAL-size check). */
    static FILE *g_dump = NULL; static int g_dump_init = 0;
    if (!g_dump_init) { g_dump_init = 1; const char *dp = getenv("BSDR_DUMP_CLOUD"); if (dp) g_dump = fopen(dp, "wb"); }
    if (g_dump) { fwrite(annexb, 1, len, g_dump); fflush(g_dump); }
    uint16_t w = align16(src_w), h = align16(src_h);
    uint64_t now = mono_ms();
    uint8_t ts_delta = v->cloud_prev_ms ? (uint8_t)(now - v->cloud_prev_ms) : 33;
    v->cloud_prev_ms = now;
    const uint8_t *end = annexb + len;
    int sclen = 0;
    const uint8_t *sc = next_start(annexb, end, &sclen);
    /* PACING: spread the keyframe burst (~6 pkts/ms) so it doesn't overflow the Quest's WiFi. */
    int paced = 0;
    while (sc) {
        const uint8_t *nal = sc + sclen;
        int nx = 0;
        const uint8_t *nsc = next_start(nal, end, &nx);
        const uint8_t *nal_end = nsc ? nsc : end;
        size_t nlen = (size_t)(nal_end - nal);
        if (nlen > 0 && nal_is_keepable(nal[0])) {
            /* Each NAL (SPS/PPS/IDR/slice) gets exactly ONE frame_id. The Quest bootstraps a keyframe
             * by finding three CONSECUTIVE frame_ids typed SPS(7),PPS(8),IDR(5) (confirmed in
             * libBigSoup.so video_stream.cpp:105); duplicating SPS/PPS into extra frame_ids breaks
             * that window -> permanent buffering. So send each NAL once, never twice. */
            uint16_t total = (uint16_t)((nlen + CLOUD_FRAG_NAL - 1) / CLOUD_FRAG_NAL);
            if (total == 0) total = 1;
            uint32_t frame = v->cloud_frame++;
            for (uint16_t fi = 0; fi < total; fi++) {
                size_t off  = (size_t)fi * CLOUD_FRAG_NAL;
                size_t clen = nlen - off;
                if (clen > CLOUD_FRAG_NAL) clen = CLOUD_FRAG_NAL;
                if (cloud_send_frag(v, nal + off, clen, w, h, frame, fi, total, ts_delta) < 0)
                    return -1;
                if (++paced % 6 == 0) {           /* spread the burst */
                    struct timespec ps = { 0, 1000000 };   /* 1 ms */
                    nanosleep(&ps, NULL);
                }
            }
        }
        sc = nsc; sclen = nx;
    }
    return 0;
}

/* Packetize one NAL unit (RFC 6184: single NAL or FU-A). */
static int send_nal(bsdr_video_sender *v, const uint8_t *nal, size_t len,
                    uint32_t ts, int last_in_au) {
    if (len == 0) return 0;
    if (len <= MAX_RTP_PAYLOAD) {
        return send_rtp(v, nal, len, ts, last_in_au);
    }
    /* FU-A: split the NAL payload (after the 1-byte NAL header) into fragments. */
    uint8_t nal_hdr = nal[0];
    uint8_t fu_indicator = (uint8_t)((nal_hdr & 0xe0) | 28);   /* type 28 = FU-A */
    uint8_t nal_type = nal_hdr & 0x1f;
    const uint8_t *data = nal + 1;
    size_t remaining = len - 1;
    uint8_t frag[2 + MAX_RTP_PAYLOAD];
    int first = 1;
    while (remaining > 0) {
        size_t chunk = remaining > (MAX_RTP_PAYLOAD - 2) ? (MAX_RTP_PAYLOAD - 2) : remaining;
        int last_frag = (chunk == remaining);
        frag[0] = fu_indicator;
        frag[1] = (uint8_t)((first ? 0x80 : 0) | (last_frag ? 0x40 : 0) | nal_type);
        memcpy(frag + 2, data, chunk);
        if (send_rtp(v, frag, chunk + 2, ts, last_in_au && last_frag) < 0) return -1;
        data += chunk; remaining -= chunk; first = 0;
    }
    return 0;
}

/* Iterate Annex-B start codes (00 00 01 / 00 00 00 01). */
static const uint8_t *next_start(const uint8_t *p, const uint8_t *end, int *sclen) {
    for (const uint8_t *q = p; q + 3 <= end; q++) {
        if (q[0] == 0 && q[1] == 0 && q[2] == 1) { *sclen = 3; return q; }
        if (q + 4 <= end && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
            *sclen = 4; return q;
        }
    }
    return NULL;
}

/* The Quest's hardware H.264 decoder (Android MediaCodec) crashes on NAL units the
 * real host's OpenH264 Constrained-Baseline stream never contains. h264_nvenc, in
 * CBR, emits filler (type 12) every frame plus a pic-timing SEI (type 6) before each
 * frame — both fatal to the headset. Drop everything that isn't a parameter set or a
 * coded slice so we ship the same minimal stream the real host does. */
static int nal_is_keepable(uint8_t nal_hdr) {
    switch (nal_hdr & 0x1f) {
        case 1:  /* non-IDR slice (P) */
        case 5:  /* IDR slice        */
        case 7:  /* SPS              */
        case 8:  /* PPS              */
            return 1;
        default: /* 6=SEI 9=AUD 12=filler 13=SPS-ext etc. -> drop */
            return 0;
    }
}

int bsdr_video_send_access_unit(bsdr_video_sender *v, const uint8_t *annexb,
                                size_t len, uint32_t rtp_ts) {
    const uint8_t *end = annexb + len;
    int sclen = 0;
    const uint8_t *sc = next_start(annexb, end, &sclen);
    if (!sc) return -1;
    /* Walk NALs with a one-unit delay so the RTP marker lands on the last *kept*
     * NAL of the access unit (not on a trailing filler we just dropped). */
    const uint8_t *nal = sc + sclen;
    const uint8_t *pend = NULL; size_t pend_len = 0;
    while (nal < end) {
        int next_sclen = 0;
        const uint8_t *nsc = next_start(nal, end, &next_sclen);
        const uint8_t *nal_end = nsc ? nsc : end;
        size_t nlen = (size_t)(nal_end - nal);
        if (nlen > 0 && nal_is_keepable(nal[0])) {
            if (pend && send_nal(v, pend, pend_len, rtp_ts, 0) < 0) return -1;
            pend = nal; pend_len = nlen;
        }
        if (!nsc) break;
        nal = nsc + next_sclen;
    }
    if (pend && send_nal(v, pend, pend_len, rtp_ts, 1) < 0) return -1;  /* marker=last */
    return 0;
}


void bsdr_video_sender_free(bsdr_video_sender *v) {
    if (!v) return;
    if (!v->plain) { srtp_dealloc(v->srtp); bsdr_srtp_global_shutdown(); }
    free(v);
}

/* ------------------------------------------------------------------ depay ---*/
struct bsdr_h264_depay {
    uint8_t fu_buf[1024 * 1024];
    size_t fu_len;
    int in_fu;
};

bsdr_h264_depay *bsdr_h264_depay_new(void) {
    return calloc(1, sizeof(struct bsdr_h264_depay));
}
void bsdr_h264_depay_free(bsdr_h264_depay *d) { free(d); }

static int append_nal(uint8_t *out, size_t outcap, size_t *outlen,
                      const uint8_t *nal, size_t len) {
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    if (*outlen + 4 + len > outcap) return -1;
    memcpy(out + *outlen, sc, 4); *outlen += 4;
    memcpy(out + *outlen, nal, len); *outlen += len;
    return 0;
}

int bsdr_h264_depay_feed(bsdr_h264_depay *d, const uint8_t *payload, size_t len,
                         uint8_t *out, size_t outcap, size_t *outlen) {
    if (len < 1) return (int)*outlen;
    uint8_t type = payload[0] & 0x1f;
    if (type >= 1 && type <= 23) {            /* single NAL */
        append_nal(out, outcap, outlen, payload, len);
    } else if (type == 28) {                  /* FU-A */
        if (len < 2) return (int)*outlen;
        uint8_t fu = payload[1];
        int start = fu & 0x80, end = fu & 0x40;
        if (start) {
            d->fu_len = 0; d->in_fu = 1;
            d->fu_buf[d->fu_len++] = (uint8_t)((payload[0] & 0xe0) | (fu & 0x1f));
        }
        if (d->in_fu && d->fu_len + (len - 2) < sizeof(d->fu_buf)) {
            memcpy(d->fu_buf + d->fu_len, payload + 2, len - 2);
            d->fu_len += len - 2;
        }
        if (end && d->in_fu) {
            append_nal(out, outcap, outlen, d->fu_buf, d->fu_len);
            d->in_fu = 0;
        }
    }
    return (int)*outlen;
}
