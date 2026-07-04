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
/* Local H.264 file -> Annex-B access units (no re-encode). */
#include "bsdr/filesrc.h"
#include "bsdr/protocol.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>

#include <stdlib.h>
#include <string.h>

struct bsdr_filesrc {
    AVFormatContext *fmt;
    AVBSFContext *bsf;     /* h264_mp4toannexb, or NULL for raw/TS */
    AVPacket *pkt;         /* demuxed */
    AVPacket *fpkt;        /* bitstream-filtered (the one we return) */
    int vstream;
    bool loop;
    int width, height;
    double fps;
    uint64_t start_ms;     /* wall clock at first frame */
    int64_t first_pts;     /* stream-units pts of first frame */
};

bsdr_filesrc *bsdr_filesrc_open(const char *path, bool loop) {
    bsdr_filesrc *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->loop = loop;
    f->start_ms = 0;
    f->first_pts = AV_NOPTS_VALUE;

    if (avformat_open_input(&f->fmt, path, NULL, NULL) != 0) {
        BSDR_ERROR("bsdr.file", "cannot open %s", path); goto fail;
    }
    if (avformat_find_stream_info(f->fmt, NULL) < 0) goto fail;
    f->vstream = av_find_best_stream(f->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (f->vstream < 0) { BSDR_ERROR("bsdr.file", "no video stream"); goto fail; }

    AVStream *st = f->fmt->streams[f->vstream];
    AVCodecParameters *par = st->codecpar;
    if (par->codec_id != AV_CODEC_ID_H264) {
        BSDR_ERROR("bsdr.file", "not H.264 (passthrough needs H.264; transcode TODO)");
        goto fail;
    }
    f->width = par->width;
    f->height = par->height;
    f->fps = st->avg_frame_rate.num ? av_q2d(st->avg_frame_rate) : 30.0;

    /* MP4/MOV store AVCC (length-prefixed) -> convert to Annex-B. Raw/TS streams
     * are already Annex-B (extradata absent or not AVCC). */
    if (par->extradata_size > 0 && par->extradata[0] == 1) {
        const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if (bsf && av_bsf_alloc(bsf, &f->bsf) == 0) {
            avcodec_parameters_copy(f->bsf->par_in, par);
            f->bsf->time_base_in = st->time_base;
            if (av_bsf_init(f->bsf) < 0) { av_bsf_free(&f->bsf); f->bsf = NULL; }
        }
    }
    f->pkt = av_packet_alloc();
    f->fpkt = av_packet_alloc();
    BSDR_INFO("bsdr.file", "streaming %s: H.264 %dx%d @%.1ffps%s",
              path, f->width, f->height, f->fps, f->bsf ? " (mp4->annexb)" : "");
    return f;
fail:
    bsdr_filesrc_close(f);
    return NULL;
}

static uint32_t pts_to_rtp(bsdr_filesrc *f, int64_t pts) {
    AVStream *st = f->fmt->streams[f->vstream];
    if (pts == AV_NOPTS_VALUE) pts = 0;
    return (uint32_t)av_rescale_q(pts, st->time_base, (AVRational){ 1, BSDR_VIDEO_CLOCK_HZ });
}

/* sleep so the AU is emitted at its presentation time relative to the first. */
static void pace(bsdr_filesrc *f, int64_t pts) {
    AVStream *st = f->fmt->streams[f->vstream];
    if (pts == AV_NOPTS_VALUE) return;
    if (f->first_pts == AV_NOPTS_VALUE) { f->first_pts = pts; f->start_ms = bsdr_now_ms(); return; }
    double rel = av_q2d(st->time_base) * (double)(pts - f->first_pts);
    uint64_t target = f->start_ms + (uint64_t)(rel * 1000.0);
    uint64_t now = bsdr_now_ms();
    if (target > now && target - now < 2000) bsdr_sleep_ms((unsigned)(target - now));
}

int bsdr_filesrc_frame(bsdr_filesrc *f, const uint8_t **au, size_t *len,
                       uint32_t *rtp_ts) {
    for (;;) {
        int r = av_read_frame(f->fmt, f->pkt);
        if (r < 0) {
            if (f->loop && av_seek_frame(f->fmt, f->vstream, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                f->first_pts = AV_NOPTS_VALUE;   /* restart pacing */
                continue;
            }
            return -1;   /* EOF */
        }
        if (f->pkt->stream_index != f->vstream) { av_packet_unref(f->pkt); continue; }

        AVPacket *out = f->pkt;
        if (f->bsf) {
            if (av_bsf_send_packet(f->bsf, f->pkt) < 0) { av_packet_unref(f->pkt); continue; }
            if (av_bsf_receive_packet(f->bsf, f->fpkt) < 0) { continue; }  /* need more input */
            out = f->fpkt;
        }
        pace(f, out->pts);
        *au = out->data;
        *len = (size_t)out->size;
        *rtp_ts = pts_to_rtp(f, out->pts);
        /* `out` stays valid until the next call, which unrefs it via
         * av_read_frame (non-BSF) or av_bsf_receive_packet (BSF). */
        return 1;
    }
}

void bsdr_filesrc_seek(bsdr_filesrc *f, double frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int64_t dur = f->fmt->duration;          /* AV_TIME_BASE units */
    if (dur <= 0) return;
    int64_t ts = (int64_t)(frac * dur);
    AVStream *st = f->fmt->streams[f->vstream];
    int64_t stream_ts = av_rescale_q(ts, (AVRational){1, AV_TIME_BASE}, st->time_base);
    if (av_seek_frame(f->fmt, f->vstream, stream_ts, AVSEEK_FLAG_BACKWARD) >= 0)
        f->first_pts = AV_NOPTS_VALUE;       /* restart pacing */
}

void bsdr_filesrc_info(bsdr_filesrc *f, int *w, int *h, double *fps) {
    if (w) *w = f->width;
    if (h) *h = f->height;
    if (fps) *fps = f->fps;
}

void bsdr_filesrc_close(bsdr_filesrc *f) {
    if (!f) return;
    if (f->fpkt) av_packet_free(&f->fpkt);
    if (f->pkt) av_packet_free(&f->pkt);
    if (f->bsf) av_bsf_free(&f->bsf);
    if (f->fmt) avformat_close_input(&f->fmt);
    free(f);
}
