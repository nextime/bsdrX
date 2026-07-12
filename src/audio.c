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
/* Audio: Opus over RTP/SRTP (RFC 7587) + PulseAudio I/O + virtual devices. */
#include "bsdr/audio.h"
#include "bsdr/srtp_util.h"
#include "bsdr/protocol.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <opus/opus.h>
/* PulseAudio device half: Linux only. Windows (audio_wasapi.c), macOS (audio_coreaudio.c) and
 * Android (audio_android.c) provide bsdr_pa_* / virtual devices for their platforms; the Opus
 * sender/receiver/player below are shared across all of them. */
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RTP_HDR 12

static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v & 0xff;
}

/* ------------------------------------------------------------------ sender */
struct bsdr_audio_sender {
    bsdr_udp *udp;
    srtp_t srtp;
    int plain;         /* NULL keys => plain RTP (cloud Mediasoup path) */
    OpusEncoder *enc;
    uint8_t pt;
    uint32_t ssrc, ts;
    uint16_t seq;
    int channels;
    int first;
    int gain;          /* 0..100 */
    int cloud_trailer; /* append the 8-byte BigSoup cloud trailer [u32 ssrc LE][u32 frame_id LE] */
    uint32_t cloud_frame_id;
    int16_t scaled[BSDR_OPUS_FRAME * 2];
    uint8_t pkt[RTP_HDR + 1500 + SRTP_MAX_TRAILER_LEN];
};

/* Cloud desktop audio: the Quest reads the producer SSRC + frame id from an 8-byte trailer appended
 * after the Opus payload (reversed from libBigSoup.so audio_stream.cpp:248). Without it the consumer
 * reads the last 8 Opus bytes as SSRC (=0 on quiet frames), registers a phantom user, and double-frees
 * its audio sink -> SIGABRT. ssrc must equal the RTP/video SSRC (djb2(userSessionId)); NO XOR. */
void bsdr_audio_sender_enable_cloud_trailer(bsdr_audio_sender *s) { s->cloud_trailer = 1; }

void bsdr_audio_sender_set_gain(bsdr_audio_sender *s, int vol) {
    s->gain = vol < 0 ? 0 : vol > 100 ? 100 : vol;
}

bsdr_audio_sender *bsdr_audio_sender_new(bsdr_udp *udp, const bsdr_srtp_keys *keys,
                                         uint8_t pt, uint32_t ssrc,
                                         int channels, int bitrate) {
    bool plain = (keys == NULL);   /* plain RTP (cloud relay) needs no libsrtp */
    if (!plain && !bsdr_srtp_global_init()) return NULL;
    bsdr_audio_sender *s = calloc(1, sizeof(*s));
    if (!s) { if (!plain) bsdr_srtp_global_shutdown(); return NULL; }
    s->udp = udp;
    s->pt = pt ? pt : BSDR_AUDIO_PT_DEFAULT;
    s->ssrc = ssrc;
    s->channels = channels;
    s->first = 1;
    s->gain = 100;
    /* RFC 3550 / match the official host: start seq + timestamp at random values. A producer
     * whose RTP starts at seq 0 / ts 0 can be mishandled by mediasoup (we already do this for
     * video; the audio sender was starting at 0/0, unlike the official client which starts high). */
    struct timespec rt; clock_gettime(CLOCK_MONOTONIC, &rt);
    uint32_t r = (uint32_t)(rt.tv_nsec ^ ((uint32_t)rt.tv_sec << 19) ^ (uint32_t)((uintptr_t)s >> 4));
    s->seq = (uint16_t)(r & 0xffff);
    s->ts  = r * 2654435761u;
    int err = 0;
    s->enc = opus_encoder_create(BSDR_AUDIO_CLOCK_HZ, channels,
                                 OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !s->enc) { BSDR_ERROR("bsdr.audio", "opus enc create"); goto fail; }
    opus_encoder_ctl(s->enc, OPUS_SET_BITRATE(bitrate));
    s->plain = plain;
    if (!s->plain &&
        !bsdr_srtp_session_create(&s->srtp, keys->send_master, keys->profile, ssrc, false)) {
        BSDR_ERROR("bsdr.audio", "srtp session"); goto fail;
    }
    BSDR_INFO("bsdr.audio", "Opus %s sender ready (pt=%u ssrc=%08x %dch %dbps)",
              s->plain ? "PLAIN-RTP" : "SRTP", s->pt, ssrc, channels, bitrate);
    return s;
fail:
    if (s->enc) opus_encoder_destroy(s->enc);
    free(s); if (!plain) bsdr_srtp_global_shutdown();
    return NULL;
}

int bsdr_audio_send_pcm(bsdr_audio_sender *s, const int16_t *pcm, int frames) {
    if (s->gain != 100) {                 /* apply volume */
        int total = frames * s->channels;
        if (total > (int)(sizeof(s->scaled) / sizeof(s->scaled[0]))) total = sizeof(s->scaled)/sizeof(s->scaled[0]);
        for (int i = 0; i < total; i++) s->scaled[i] = (int16_t)(pcm[i] * s->gain / 100);
        pcm = s->scaled;
    }
    uint8_t *p = s->pkt;
    int n = opus_encode(s->enc, pcm, frames, p + RTP_HDR,
                        (int)sizeof(s->pkt) - RTP_HDR - SRTP_MAX_TRAILER_LEN - 8);
    if (n < 0) return -1;
    if (s->cloud_trailer) {            /* [u32 ssrc LE][u32 frame_id LE], no XOR, after the Opus payload */
        uint8_t *tr = p + RTP_HDR + n;
        uint32_t id = ++s->cloud_frame_id;   /* strictly increasing, starts at 1 */
        tr[0]=s->ssrc; tr[1]=s->ssrc>>8; tr[2]=s->ssrc>>16; tr[3]=s->ssrc>>24;
        tr[4]=id; tr[5]=id>>8; tr[6]=id>>16; tr[7]=id>>24;
        n += 8;
    }
    p[0] = 0x80;
    p[1] = (uint8_t)(s->pt & 0x7f);   /* marker always 0 — matches the official host's audio */
    s->first = 0;
    wr16(p + 2, s->seq++);
    wr32(p + 4, s->ts);
    wr32(p + 8, s->ssrc);
    s->ts += (uint32_t)frames;
    int len = RTP_HDR + n;
    if (!s->plain && srtp_protect(s->srtp, p, &len) != srtp_err_status_ok) return -1;
    return bsdr_udp_send(s->udp, p, (size_t)len) < 0 ? -1 : 0;
}

void bsdr_audio_sender_free(bsdr_audio_sender *s) {
    if (!s) return;
    if (!s->plain) { srtp_dealloc(s->srtp); bsdr_srtp_global_shutdown(); }
    opus_encoder_destroy(s->enc);
    free(s);
}

/* ---------------------------------------------------------------- receiver */
/* Per-SSRC mic streams with a jitter buffer + Opus PLC, mixed into one frame — matching BigSoup's
 * incoming-mic handling (one Opus decoder per remote SSRC; reorder + expire late frames, "Mic audio
 * frame %u was expired"; multiple incoming streams summed). feed() only buffers; playout() (on a
 * ~20 ms clock) decodes the next in-order frame of each stream (or PLC for a gap) and sums them. */
#define BSDR_MIC_STREAMS 8
#define BSDR_JB_SLOTS    48      /* jitter ring; must exceed the reorder window + max target */
/* Adaptive playout delay (in 20 ms frames): grows on late/underrun jitter, shrinks after a clean
 * run. Default ~40 ms; bounded [20 ms, 240 ms]. */
#define BSDR_JB_MIN      1
#define BSDR_JB_MAX      12
#define BSDR_JB_DEFAULT  2
#define BSDR_JB_SHRINK   250    /* clean in-order frames (~5 s) before trimming one frame of delay */

/* data holds one Opus RTP payload; a 20 ms Opus frame maxes at ~1276 B (510 kbps), so 1300 is a safe
 * ceiling (was 1500 = MTU-sized, ~200 B/slot of waste). Oversized packets were already dropped. */
typedef struct { uint8_t data[1300]; int len; uint16_t seq; bool present; } bsdr_jb_slot;

typedef struct {
    uint32_t     ssrc;
    OpusDecoder *dec;
    uint64_t     used_at;               /* LRU */
    bsdr_jb_slot *jb;                   /* [BSDR_JB_SLOTS]; allocated lazily when the stream first
                                         * appears (a fresh recv holds none), freed on eviction/close */
    int          count;                 /* buffered (present) packets */
    uint16_t     play_seq;              /* next RTP sequence number to play out */
    bool         started;               /* playout running (prebuffer reached) */
    int          target;                /* adaptive playout delay, in frames */
    int          holds;                 /* consecutive underrun holds at play_seq */
    int          good;                  /* consecutive clean in-order frames (for shrinking) */
    float        energy;                /* smoothed loudness (for the voice-activity duck) */
} bsdr_mic_stream;

struct bsdr_audio_recv {
    srtp_t srtp;
    int plain;         /* NULL keys => plain RTP */
    uint8_t pt;
    int channels;
    bsdr_pcm_cb cb;
    void *user;
    uint64_t tick;     /* monotonic counter for LRU eviction */
    volatile int duck; /* voice-activity duck: mix ONLY the loudest stream, mute the rest (owner
                        * isolation while a voice command is captured over the cloud fallback). */
    volatile uint32_t solo_ssrc; /* if non-zero, mix ONLY this SSRC (identity solo: "listen only to
                        * <that participant>", e.g. the room owner) — mute every other stream. */
    int cloud_trailer; /* strip the 8-byte BigSoup trailer [u32 ssrc][u32 frame_id] before decode */
    bsdr_mic_stream streams[BSDR_MIC_STREAMS];
    int16_t pcm[BSDR_OPUS_FRAME * 2];   /* one mixed 20 ms frame (interleaved) */
};

/* Find or create the per-SSRC stream (evicting the LRU one if full). */
static bsdr_mic_stream *mic_stream_for(bsdr_audio_recv *r, uint32_t ssrc) {
    uint64_t now = ++r->tick;
    int lru = 0;
    for (int i = 0; i < BSDR_MIC_STREAMS; i++) {
        if (r->streams[i].dec && r->streams[i].ssrc == ssrc) { r->streams[i].used_at = now; return &r->streams[i]; }
        if (r->streams[i].used_at < r->streams[lru].used_at) lru = i;
    }
    int slot = -1;
    for (int i = 0; i < BSDR_MIC_STREAMS; i++) if (!r->streams[i].dec) { slot = i; break; }
    if (slot < 0) slot = lru;
    bsdr_mic_stream *st = &r->streams[slot];
    if (st->dec) opus_decoder_destroy(st->dec);
    bsdr_jb_slot *jb = st->jb;                       /* reuse this slot's ring if it already had one */
    memset(st, 0, sizeof(*st));
    if (!jb) jb = calloc(BSDR_JB_SLOTS, sizeof(bsdr_jb_slot));
    else     memset(jb, 0, (size_t)BSDR_JB_SLOTS * sizeof(bsdr_jb_slot));
    if (!jb) return NULL;
    st->jb = jb;
    int err = 0;
    st->dec = opus_decoder_create(BSDR_AUDIO_CLOCK_HZ, r->channels, &err);
    if (err != OPUS_OK || !st->dec) { free(st->jb); st->jb = NULL; st->dec = NULL; return NULL; }
    st->ssrc = ssrc; st->used_at = now; st->target = BSDR_JB_DEFAULT;
    BSDR_INFO("bsdr.audio", "mic: new incoming stream SSRC=%u (slot %d)", ssrc, slot);
    return st;
}

bsdr_audio_recv *bsdr_audio_recv_new(const bsdr_srtp_keys *keys, uint8_t pt,
                                     uint32_t ssrc, int channels,
                                     bsdr_pcm_cb cb, void *user) {
    bool plain = (keys == NULL);   /* plain RTP (cloud relay) needs no libsrtp */
    if (!plain && !bsdr_srtp_global_init()) return NULL;
    bsdr_audio_recv *r = calloc(1, sizeof(*r));
    if (!r) { if (!plain) bsdr_srtp_global_shutdown(); return NULL; }
    r->pt = pt ? pt : BSDR_AUDIO_PT_DEFAULT;
    r->channels = channels;
    r->cb = cb; r->user = user;
    /* decoders + jitter buffers are created lazily per incoming SSRC (mic_stream_for) */
    r->plain = plain;
    if (!r->plain &&
        !bsdr_srtp_session_create(&r->srtp, keys->recv_master, keys->profile, ssrc, true)) {
        free(r); bsdr_srtp_global_shutdown(); return NULL;
    }
    return r;
}

/* Buffer one inbound RTP packet into its SSRC's jitter buffer. Decoding happens in playout(). */
int bsdr_audio_recv_feed(bsdr_audio_recv *r, uint8_t *pkt, int len) {
    if (len < RTP_HDR) return 0;
    if ((pkt[1] & 0x7f) != r->pt) return 0;          /* not our audio PT */
    if (!r->plain && srtp_unprotect(r->srtp, pkt, &len) != srtp_err_status_ok) return 0;
    /* RTP header is 12 + 4*CC, plus a header extension when X (byte0 bit 4) is set.
     * The Mediasoup relay sends Opus with an extension (byte0=0x90), so skip it. */
    int hdr = RTP_HDR + 4 * (pkt[0] & 0x0f);
    if ((pkt[0] & 0x10) && len >= hdr + 4) {         /* X bit: extension present */
        int extwords = (pkt[hdr + 2] << 8) | pkt[hdr + 3];
        hdr += 4 + 4 * extwords;
    }
    if (len <= hdr) return 0;
    int plen = len - hdr;
    if (r->cloud_trailer && plen > 8) plen -= 8;   /* drop the 8-byte [ssrc][frame_id] cloud trailer */
    if (plen > (int)sizeof(r->streams[0].jb[0].data)) return 0;
    uint16_t seq  = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint32_t ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                    ((uint32_t)pkt[10] << 8) | (uint32_t)pkt[11];
    bsdr_mic_stream *st = mic_stream_for(r, ssrc);
    if (!st) return 0;

    if (!st->started && st->count == 0) st->play_seq = seq;          /* anchor a fresh burst */
    if (st->started) {
        if ((int16_t)(seq - st->play_seq) < 0) {                     /* late — already played out */
            if (st->target < BSDR_JB_MAX) st->target++;             /* grow the buffer to catch it next time */
            st->good = 0;
            return 0;
        }
        if ((uint16_t)(seq - st->play_seq) >= BSDR_JB_SLOTS) return 0; /* too far ahead — drop */
    }
    bsdr_jb_slot *sl = &st->jb[seq % BSDR_JB_SLOTS];
    if (!(sl->present && sl->seq == seq)) {                          /* ignore duplicates */
        if (!sl->present) st->count++;
        memcpy(sl->data, pkt + hdr, (size_t)plen);
        sl->len = plen; sl->seq = seq; sl->present = true;
    }
    if (!st->started && st->count >= st->target) st->started = true;
    return 1;
}

/* Play out one 20 ms frame: decode each active stream's next packet (or Opus PLC for a gap), sum
 * the streams, and emit one mixed frame via the callback. Call on a ~20 ms clock. */
int bsdr_audio_recv_playout(bsdr_audio_recv *r) {
    const int ns = BSDR_OPUS_FRAME * r->channels;
    int32_t mix[BSDR_OPUS_FRAME * 2] = {0};
    int16_t tmp[BSDR_OPUS_FRAME * 2];
    bool any = false;
    /* Voice-activity duck: pick the loudest active stream (the person speaking = the owner giving a
     * command) from the previous tick's energies; mix only it and mute the rest. ~1 frame of lag to
     * lock on, imperceptible for a command. */
    bsdr_mic_stream *speaker = NULL;
    if (r->duck) {
        float best = -1.0f;
        for (int s = 0; s < BSDR_MIC_STREAMS; s++) {
            bsdr_mic_stream *st = &r->streams[s];
            if (st->dec && st->started && st->energy > best) { best = st->energy; speaker = st; }
        }
    }
    for (int s = 0; s < BSDR_MIC_STREAMS; s++) {
        bsdr_mic_stream *st = &r->streams[s];
        if (!st->dec || !st->started) continue;
        /* Drain latency built up by earlier holds: if buffered well beyond target, consume one
         * extra frame this tick (decode it to keep Opus state, but don't output it). */
        if (st->count > st->target + 2) {
            bsdr_jb_slot *d = &st->jb[st->play_seq % BSDR_JB_SLOTS];
            if (d->present && d->seq == st->play_seq) {
                int dn = opus_decode(st->dec, d->data, d->len, tmp, BSDR_OPUS_FRAME, 0);
                (void)dn;   /* decoded only to keep Opus state; output discarded */
                d->present = false; st->count--;
            }
            st->play_seq++;
        }
        bsdr_jb_slot *sl = &st->jb[st->play_seq % BSDR_JB_SLOTS];
        int frames;
        if (sl->present && sl->seq == st->play_seq) {             /* in-order frame */
            frames = opus_decode(st->dec, sl->data, sl->len, tmp, BSDR_OPUS_FRAME, 0);
            sl->present = false; st->count--; st->play_seq++; st->holds = 0;
            /* a clean run lets us trim one frame of latency (shrink toward BSDR_JB_MIN) */
            if (++st->good >= BSDR_JB_SHRINK && st->target > BSDR_JB_MIN) { st->target--; st->good = 0; }
        } else if (st->count > 0) {                               /* gap, real packets ahead → lost; PLC */
            frames = opus_decode(st->dec, NULL, 0, tmp, BSDR_OPUS_FRAME, 0);
            st->play_seq++; st->holds = 0; st->good = 0;
        } else if (st->holds < st->target) {                      /* underrun: hold (don't advance) and
                                                                   * conceal, giving late packets time —
                                                                   * this is the adaptive grow at runtime */
            frames = opus_decode(st->dec, NULL, 0, tmp, BSDR_OPUS_FRAME, 0);
            if (st->holds == 0 && st->target < BSDR_JB_MAX) st->target++;   /* deepen on a new underrun */
            st->holds++; st->good = 0;
        } else {                                                  /* drained → pause until new pkts */
            st->started = false; st->holds = 0;
            continue;
        }
        if (frames > 0) {
            /* update smoothed loudness for the duck's speaker pick (next tick) */
            int64_t e = 0; int n = frames * r->channels;
            for (int i = 0; i < n; i++) { int v = tmp[i]; e += v < 0 ? -v : v; }
            st->energy = st->energy * 0.6f + ((float)e / (float)n) * 0.4f;
            /* solo: include only the chosen SSRC; duck: include only the loudest; else mix all. */
            bool include = (!r->duck || st == speaker);
            if (r->solo_ssrc && st->ssrc != r->solo_ssrc) include = false;
            if (include) {
                for (int i = 0; i < n; i++) mix[i] += tmp[i];
                any = true;
            }
        }
    }
    if (!any) return 0;
    for (int i = 0; i < ns; i++) {
        int32_t v = mix[i];
        r->pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    if (r->cb) r->cb(r->pcm, BSDR_OPUS_FRAME, r->channels, r->user);
    return 1;
}

void bsdr_audio_recv_set_duck(bsdr_audio_recv *r, int on) {
    if (r) r->duck = on ? 1 : 0;
}
void bsdr_audio_recv_set_solo(bsdr_audio_recv *r, uint32_t ssrc) {
    if (r) r->solo_ssrc = ssrc;   /* 0 = mix everyone; else mix only this SSRC */
}
void bsdr_audio_recv_enable_cloud_trailer(bsdr_audio_recv *r) {
    if (r) r->cloud_trailer = 1;
}

void bsdr_audio_recv_free(bsdr_audio_recv *r) {
    if (!r) return;
    if (!r->plain) { srtp_dealloc(r->srtp); bsdr_srtp_global_shutdown(); }
    for (int i = 0; i < BSDR_MIC_STREAMS; i++) {
        if (r->streams[i].dec) opus_decoder_destroy(r->streams[i].dec);
        free(r->streams[i].jb);
    }
    free(r);
}

/* -------------------------------------------------------------- PulseAudio */
/* Windows: bsdr_pa + virtual devices live in audio_wasapi.c; macOS: audio_coreaudio.c; Android: audio_android.c. */
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
struct bsdr_pa { pa_simple *s; int channels; };

static bsdr_pa *pa_open(const char *dev, int channels, pa_stream_direction_t dir) {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16NE;
    ss.rate = BSDR_AUDIO_CLOCK_HZ;
    ss.channels = (uint8_t)channels;
    /* Explicit buffering. The default pa_simple attrs give almost no slack, so a transient CPU spike
     * (encode/send hiccup) overruns the capture or starves playout -> audible crackle. Give the RECORD
     * stream ~200ms of headroom (costs no latency while we read promptly; just absorbs spikes) with
     * ~20ms fragments, and the PLAYBACK stream a ~50ms target. */
    unsigned bps = ss.rate * ss.channels * 2;     /* bytes/sec */
    pa_buffer_attr attr;
    if (dir == PA_STREAM_RECORD) {
        attr.maxlength = bps / 5;                 /* ~200ms ceiling before overrun */
        attr.fragsize  = bps / 50;                /* ~20ms fragments (low latency) */
        attr.tlength = attr.prebuf = attr.minreq = (uint32_t)-1;
    } else {
        attr.maxlength = (uint32_t)-1;
        attr.tlength   = bps / 20;                /* ~50ms playout target */
        attr.prebuf    = bps / 50;                /* ~20ms prebuffer */
        attr.minreq    = bps / 100;               /* ~10ms */
        attr.fragsize  = (uint32_t)-1;
    }
    int err = 0;
    pa_simple *s = pa_simple_new(NULL, "bsdrX",
                                 dir, dev && *dev ? dev : NULL,
                                 dir == PA_STREAM_RECORD ? "desktop-audio" : "quest-mic",
                                 &ss, NULL, &attr, &err);
    if (!s) { BSDR_ERROR("bsdr.audio", "pulse open: %s", pa_strerror(err)); return NULL; }
    bsdr_pa *pa = calloc(1, sizeof(*pa));
    pa->s = s; pa->channels = channels;
    return pa;
}
bsdr_pa *bsdr_pa_record_open(const char *source, int channels) {
    return pa_open(source, channels, PA_STREAM_RECORD);
}
bsdr_pa *bsdr_pa_play_open(const char *sink, int channels) {
    return pa_open(sink, channels, PA_STREAM_PLAYBACK);
}
int bsdr_pa_read(bsdr_pa *pa, int16_t *pcm, int frames) {
    int err = 0;
    if (pa_simple_read(pa->s, pcm, (size_t)frames * pa->channels * 2, &err) < 0) return -1;
    return frames;
}
int bsdr_pa_write(bsdr_pa *pa, const int16_t *pcm, int frames) {
    int err = 0;
    if (pa_simple_write(pa->s, pcm, (size_t)frames * pa->channels * 2, &err) < 0) return -1;
    return frames;
}
void bsdr_pa_close(bsdr_pa *pa) {
    if (!pa) return;
    if (pa->s) pa_simple_free(pa->s);
    free(pa);
}
#endif /* !_WIN32 (PulseAudio I/O) */

/* ------------------------------------------------------- threaded player ---*/
struct bsdr_audio_player {
    bsdr_pa *pa;
    int channels;
    int16_t *ring;       /* sample ring */
    size_t cap, head, tail, count;
    bsdr_mutex *lock;
    bsdr_cond *have_data;   /* signalled by _push; player sleeps on it when empty */
    bsdr_thread *thread;
    volatile int stop;
};

static void player_thread(void *arg) {
    bsdr_audio_player *p = (bsdr_audio_player *)arg;
    int16_t chunk[BSDR_OPUS_FRAME * 2];
    int frame_samples = BSDR_OPUS_FRAME * p->channels;
    while (!p->stop) {
        int got = 0;
        bsdr_mutex_lock(p->lock);
        /* Block until a push signals data (or stop). No busy-poll on silence:
         * the owner-mic sniffer starts a player before any voice is captured,
         * so this thread would otherwise spin at 200Hz for the whole session.
         * Bounded wait keeps a stuck-signal from ever wedging shutdown. */
        while (p->count == 0 && !p->stop)
            bsdr_cond_wait_ms(p->have_data, p->lock, 200);
        while (got < frame_samples && p->count > 0) {
            chunk[got++] = p->ring[p->tail];
            p->tail = (p->tail + 1) % p->cap;
            p->count--;
        }
        bsdr_mutex_unlock(p->lock);
        if (got >= p->channels) bsdr_pa_write(p->pa, chunk, got / p->channels);
    }
}

bsdr_audio_player *bsdr_audio_player_new(const char *sink, int channels) {
    bsdr_audio_player *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->pa = bsdr_pa_play_open(sink, channels);
    if (!p->pa) { free(p); return NULL; }
    p->channels = channels;
    p->cap = (size_t)(BSDR_AUDIO_CLOCK_HZ / 4) * channels;   /* ~250 ms (overflow drops oldest; 1 s was overkill) */
    p->ring = calloc(p->cap, sizeof(int16_t));
    p->lock = bsdr_mutex_new();
    p->have_data = bsdr_cond_new();
    p->thread = bsdr_thread_start(player_thread, p);
    return p;
}

void bsdr_audio_player_push(bsdr_audio_player *p, const int16_t *pcm, int frames) {
    size_t n = (size_t)frames * p->channels;
    bsdr_mutex_lock(p->lock);
    for (size_t i = 0; i < n; i++) {
        if (p->count == p->cap) {           /* overflow: drop oldest */
            p->tail = (p->tail + 1) % p->cap; p->count--;
        }
        p->ring[p->head] = pcm[i];
        p->head = (p->head + 1) % p->cap; p->count++;
    }
    bsdr_cond_signal(p->have_data);   /* wake the player if it was sleeping */
    bsdr_mutex_unlock(p->lock);
}

void bsdr_audio_player_free(bsdr_audio_player *p) {
    if (!p) return;
    bsdr_mutex_lock(p->lock);
    p->stop = 1;
    bsdr_cond_signal(p->have_data);   /* unblock the player so it can exit */
    bsdr_mutex_unlock(p->lock);
    if (p->thread) bsdr_thread_join(p->thread);
    bsdr_pa_close(p->pa);
    bsdr_cond_free(p->have_data);
    bsdr_mutex_free(p->lock);
    free(p->ring);
    free(p);
}

/* ---------------------------------------------------------- virtual devices */
/* Windows: bsdr_audio_devices_create in audio_wasapi.c; Android: audio_android.c. */
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
static int pactl_load(const char *args) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pactl load-module %s 2>/dev/null", args);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char line[64]; int id = -1;
    if (fgets(line, sizeof(line), f)) id = atoi(line);
    pclose(f);
    return id;
}
static void pactl_unload(int module) {
    if (module < 0) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "pactl unload-module %d 2>/dev/null", module);
    if (system(cmd) != 0) { /* best effort */ }
}

/* Unload any leftover bsdr_* virtual devices from a previous run that didn't clean up (killed,
 * Ctrl-C force-quit, or crashed). PulseAudio keeps them loaded forever otherwise, so they pile up
 * and apps may latch a stale, dead BSDR_QuestMic. Self-healing: run this before (re)creating. */
static void pactl_unload_stale(void) {
    FILE *f = popen("pactl list short modules 2>/dev/null", "r");
    if (!f) return;
    char line[1024];
    int ids[64], n = 0;
    while (fgets(line, sizeof(line), f) && n < 64) {
        if (strstr(line, "bsdr_speaker") || strstr(line, "bsdr_micsink") ||
            strstr(line, "bsdr_quest_mic")) {
            int id = atoi(line);   /* module id = first column */
            if (id > 0) ids[n++] = id;
        }
    }
    pclose(f);
    for (int i = 0; i < n; i++) pactl_unload(ids[i]);
    if (n) BSDR_INFO("bsdr.audio", "cleaned up %d leftover virtual-device module(s)", n);
}

void bsdr_audio_cleanup_stale_devices(void) { pactl_unload_stale(); }

bool bsdr_audio_devices_create(bsdr_audio_devices *d) {
    memset(d, 0, sizeof(*d));
    d->speaker_module = d->mic_sink_module = d->mic_source_module = -1;
    pactl_unload_stale();   /* clear any leaked modules from a previous run first */

    /* remember + redirect the default sink so the local speakers go silent */
    FILE *f = popen("pactl get-default-sink 2>/dev/null", "r");
    if (f) { if (fgets(d->prev_default_sink, sizeof(d->prev_default_sink), f))
                 d->prev_default_sink[strcspn(d->prev_default_sink, "\n")] = 0;
             pclose(f); }

    d->speaker_module = pactl_load(
        "module-null-sink sink_name=bsdr_speaker "
        "sink_properties=device.description=BSDR-Quest-Speaker");
    if (d->speaker_module < 0) { BSDR_ERROR("bsdr.audio", "null-sink failed"); return false; }
    if (system("pactl set-default-sink bsdr_speaker 2>/dev/null") != 0) { /* ok */ }
    snprintf(d->monitor_source, sizeof(d->monitor_source), "bsdr_speaker.monitor");

    /* The single Quest mic. A null-sink (bsdr_micsink) that the owner-mic sniffer plays the decoded
     * headset voice into, exposed as a capture source apps record as "BSDR_QuestMic". Created with
     * the session so the device stays visible/selectable the whole time (it's silent until the owner
     * mic is started); the cloud room-audio path can also feed the same sink. */
    d->mic_sink_module = pactl_load(
        "module-null-sink sink_name=bsdr_micsink "
        "sink_properties=device.description=BSDR_QuestMicSink");
    snprintf(d->mic_sink, sizeof(d->mic_sink), "bsdr_micsink");
    d->mic_source_module = pactl_load(
        "module-remap-source master=bsdr_micsink.monitor source_name=bsdr_quest_mic "
        "source_properties=device.description=BSDR_QuestMic");

    d->active = true;
    BSDR_INFO("bsdr.audio", "virtual devices: capture %s, mic %s (apps see BSDR_QuestMic)",
              d->monitor_source, d->mic_sink);
    return true;
}

bool bsdr_virtual_mic_create(const char *sink_name, const char *source_name,
                             const char *description, int *sink_module, int *source_module) {
    char args[512];
    snprintf(args, sizeof(args),
             "module-null-sink sink_name=%s sink_properties=device.description=%s-Sink",
             sink_name, description);
    int sm = pactl_load(args);
    if (sm < 0) { BSDR_ERROR("bsdr.audio", "virtual mic: null-sink %s failed", sink_name); return false; }
    snprintf(args, sizeof(args),
             "module-remap-source master=%s.monitor source_name=%s "
             "source_properties=device.description=%s",
             sink_name, source_name, description);
    int rm = pactl_load(args);
    if (rm < 0) { BSDR_ERROR("bsdr.audio", "virtual mic: remap-source %s failed", source_name);
                  pactl_unload(sm); return false; }
    if (sink_module) *sink_module = sm;
    if (source_module) *source_module = rm;
    BSDR_INFO("bsdr.audio", "virtual mic ready: sink %s -> source %s (apps see %s)",
              sink_name, source_name, description);
    return true;
}

void bsdr_virtual_mic_destroy(int sink_module, int source_module) {
    pactl_unload(source_module);
    pactl_unload(sink_module);
}

void bsdr_audio_devices_destroy(bsdr_audio_devices *d) {
    if (!d->active) return;
    if (d->prev_default_sink[0]) {
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "pactl set-default-sink %s 2>/dev/null", d->prev_default_sink);
        if (system(cmd) != 0) { /* best effort */ }
    }
    pactl_unload(d->mic_source_module);
    pactl_unload(d->mic_sink_module);
    pactl_unload(d->speaker_module);
    d->active = false;
}
#endif /* !_WIN32 (virtual devices) */
