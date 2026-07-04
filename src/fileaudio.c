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
/* Media-file audio track -> paced interleaved S16 PCM (see fileaudio.h). */
#include "bsdr/fileaudio.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <stdlib.h>
#include <string.h>

struct bsdr_fileaudio {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int astream;
    int out_rate, out_channels;
    bool loop;
    AVRational tb;
    double duration_s;
    int64_t first_pts;      /* pts of the first presented sample block (AV_NOPTS_VALUE) */
    uint64_t start_ms;
    int16_t *buf;           /* interleaved S16 accumulator */
    int buf_cap;            /* capacity in samples (all channels) */
    int buf_len;            /* valid samples in buf */
    volatile int paused;
    volatile int seek_pending;
    double seek_frac;
};

bsdr_fileaudio *bsdr_fileaudio_open(const char *path, int out_rate, int out_channels, bool loop) {
    if (out_rate <= 0) out_rate = 48000;
    if (out_channels <= 0) out_channels = 2;
    bsdr_fileaudio *fa = calloc(1, sizeof(*fa));
    if (!fa) return NULL;
    fa->out_rate = out_rate;
    fa->out_channels = out_channels;
    fa->loop = loop;
    fa->first_pts = AV_NOPTS_VALUE;

    /* UNTRUSTED path (web UI / playlist): whitelist protocols narrowly. Local files get the tight
     * local-only set (no network SSRF, no concat/pipe arbitrary-file reads); an explicit http/https/
     * rtsp URL (validated upstream by bsdr_url_scheme_ok) also gets the network transports. */
    int is_url = path && strstr(path, "://") != NULL;
    AVDictionary *fopts = NULL;
    av_dict_set(&fopts, "protocol_whitelist",
               is_url ? "file,crypto,subfile,data,http,https,tls,tcp,rtp,rtsp,udp,hls"
                      : "file,crypto,subfile,data", 0);
    int frc = avformat_open_input(&fa->fmt, path, NULL, &fopts);
    av_dict_free(&fopts);
    if (frc != 0) goto fail;
    if (avformat_find_stream_info(fa->fmt, NULL) < 0) goto fail;
    fa->astream = av_find_best_stream(fa->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (fa->astream < 0) { BSDR_INFO("bsdr.fileaudio", "%s has no audio track", path); goto fail; }

    AVStream *st = fa->fmt->streams[fa->astream];
    fa->tb = st->time_base;
    fa->duration_s = fa->fmt->duration > 0 ? (double)fa->fmt->duration / AV_TIME_BASE : 0.0;
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { BSDR_ERROR("bsdr.fileaudio", "no audio decoder"); goto fail; }
    fa->dec = avcodec_alloc_context3(dec);
    if (!fa->dec) goto fail;
    if (avcodec_parameters_to_context(fa->dec, st->codecpar) < 0) goto fail;
    if (avcodec_open2(fa->dec, dec, NULL) < 0) goto fail;

    AVChannelLayout outlay;
    av_channel_layout_default(&outlay, out_channels);
    if (swr_alloc_set_opts2(&fa->swr, &outlay, AV_SAMPLE_FMT_S16, out_rate,
                            &fa->dec->ch_layout, fa->dec->sample_fmt, fa->dec->sample_rate,
                            0, NULL) < 0 || swr_init(fa->swr) < 0) {
        av_channel_layout_uninit(&outlay);
        BSDR_ERROR("bsdr.fileaudio", "resampler init failed"); goto fail;
    }
    av_channel_layout_uninit(&outlay);

    fa->pkt = av_packet_alloc();
    fa->frame = av_frame_alloc();
    if (!fa->pkt || !fa->frame) goto fail;
    BSDR_INFO("bsdr.fileaudio", "%s audio -> %d Hz %d ch S16 (%.1fs)", path,
              out_rate, out_channels, fa->duration_s);
    return fa;
fail:
    bsdr_fileaudio_close(fa);
    return NULL;
}

/* grow the accumulator to hold at least `need` more samples */
static int fa_reserve(bsdr_fileaudio *fa, int need) {
    if (fa->buf_len + need <= fa->buf_cap) return 1;
    int cap = fa->buf_cap ? fa->buf_cap : 8192;
    while (cap < fa->buf_len + need) cap *= 2;
    int16_t *nb = realloc(fa->buf, (size_t)cap * sizeof(int16_t));
    if (!nb) return 0;
    fa->buf = nb; fa->buf_cap = cap;
    return 1;
}

static void fa_pace(bsdr_fileaudio *fa, int64_t pts) {
    if (pts == AV_NOPTS_VALUE) return;
    if (fa->first_pts == AV_NOPTS_VALUE) { fa->first_pts = pts; fa->start_ms = bsdr_now_ms(); return; }
    double rel = av_q2d(fa->tb) * (double)(pts - fa->first_pts);
    uint64_t target = fa->start_ms + (uint64_t)(rel * 1000.0);
    uint64_t now = bsdr_now_ms();
    if (target > now && target - now < 2000) bsdr_sleep_ms((unsigned)(target - now));
}

static void fa_do_seek(bsdr_fileaudio *fa) {
    double frac = fa->seek_frac;
    fa->seek_pending = 0;
    if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
    if (fa->duration_s <= 0) return;
    int64_t ts = (int64_t)(frac * fa->duration_s / av_q2d(fa->tb));
    if (av_seek_frame(fa->fmt, fa->astream, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(fa->dec);
        fa->first_pts = AV_NOPTS_VALUE;
        fa->buf_len = 0;   /* drop stale samples so audio jumps cleanly */
    }
}

/* Resample one decoded frame into the accumulator. */
static int fa_append_frame(bsdr_fileaudio *fa) {
    int max_out = (int)av_rescale_rnd(swr_get_delay(fa->swr, fa->dec->sample_rate) + fa->frame->nb_samples,
                                      fa->out_rate, fa->dec->sample_rate, AV_ROUND_UP);
    if (max_out <= 0) return 1;
    if (!fa_reserve(fa, max_out * fa->out_channels)) return 0;
    uint8_t *dst = (uint8_t *)(fa->buf + fa->buf_len);
    int got = swr_convert(fa->swr, &dst, max_out,
                          (const uint8_t **)fa->frame->extended_data, fa->frame->nb_samples);
    if (got > 0) fa->buf_len += got * fa->out_channels;
    return 1;
}

/* Read+decode until the accumulator grows or EOF. Returns 1 on progress, 0 at true EOF. */
static int fa_fill(bsdr_fileaudio *fa) {
    for (;;) {
        int r = av_read_frame(fa->fmt, fa->pkt);
        if (r < 0) {
            if (fa->loop && av_seek_frame(fa->fmt, fa->astream, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(fa->dec); fa->first_pts = AV_NOPTS_VALUE; continue;
            }
            return 0;   /* EOF */
        }
        if (fa->pkt->stream_index != fa->astream) { av_packet_unref(fa->pkt); continue; }
        int s = avcodec_send_packet(fa->dec, fa->pkt);
        av_packet_unref(fa->pkt);
        if (s < 0) continue;
        int before = fa->buf_len;
        while (avcodec_receive_frame(fa->dec, fa->frame) == 0) {
            fa_pace(fa, fa->frame->best_effort_timestamp);
            if (!fa_append_frame(fa)) { av_frame_unref(fa->frame); return 1; }
            av_frame_unref(fa->frame);
        }
        if (fa->buf_len > before) return 1;   /* produced something */
    }
}

int bsdr_fileaudio_read(bsdr_fileaudio *fa, int16_t *pcm, int frames) {
    if (!fa) return -1;
    if (fa->paused) { bsdr_sleep_ms(10); return 0; }
    if (fa->seek_pending) fa_do_seek(fa);
    int want = frames * fa->out_channels;
    while (fa->buf_len < want) {
        if (!fa_fill(fa)) return -1;   /* EOF (non-loop) */
    }
    memcpy(pcm, fa->buf, (size_t)want * sizeof(int16_t));
    memmove(fa->buf, fa->buf + want, (size_t)(fa->buf_len - want) * sizeof(int16_t));
    fa->buf_len -= want;
    return frames;
}

void bsdr_fileaudio_seek(bsdr_fileaudio *fa, double frac) {
    if (!fa) return;
    fa->seek_frac = frac;
    fa->seek_pending = 1;   /* applied in read(), which owns the decoder */
}
void bsdr_fileaudio_set_paused(bsdr_fileaudio *fa, int paused) {
    if (fa) fa->paused = paused ? 1 : 0;
}

void bsdr_fileaudio_close(bsdr_fileaudio *fa) {
    if (!fa) return;
    if (fa->frame) av_frame_free(&fa->frame);
    if (fa->pkt) av_packet_free(&fa->pkt);
    if (fa->swr) swr_free(&fa->swr);
    if (fa->dec) avcodec_free_context(&fa->dec);
    if (fa->fmt) avformat_close_input(&fa->fmt);
    free(fa->buf);
    free(fa);
}
