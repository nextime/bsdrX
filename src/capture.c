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
/* X11 desktop capture + H.264 encode via libav (NVENC, fallback libx264). */
#include "bsdr/capture.h"
#include "bsdr/protocol.h"
#include "bsdr/overlay.h"
#include "bsdr/threed.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bsdr_capture {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVCodecContext *enc;
    struct SwsContext *sws;
    AVFrame *raw;      /* decoded screen frame */
    AVFrame *yuv;      /* NV12 fed to the encoder (CPU path). With 3D this is the packed SBS frame. */
    AVFrame *src;      /* 3D only: NV12 source (pre-SBS) the desktop is scaled into; NULL otherwise */
    AVPacket *ipkt;    /* input (raw) packet */
    AVPacket *opkt;    /* output (H.264) packet */
    int vstream;
    int fps;
    int64_t frame_index;
    const char *enc_name;
    struct bsdr_overlay *overlay;
    struct bsdr_threed *threed;    /* 2D->3D SBS synthesis (NULL = off); applied on the NV12 frame */
    /* GPU path: x11grab(CPU) -> hwupload_cuda -> scale_cuda(NV12) -> nvenc(CUDA frames). Moves the
     * BGRA->NV12 convert + resize off the CPU. use_gpu=0 => the CPU sws_scale path above. */
    int use_gpu;
    AVBufferRef *hw_device;     /* CUDA device */
    AVFilterGraph *fg;
    AVFilterContext *fg_src, *fg_sink;
    AVFrame *gpu;               /* CUDA NV12 frame out of the filter graph */
    /* ---- file-source playback (cfg.input_file) ---- */
    int is_file;                /* decoding a file rather than grabbing the screen */
    int loop;
    AVRational file_tb;         /* video stream time_base (for PTS->wall-clock pacing) */
    double duration_s;          /* file duration in seconds (0 = unknown) */
    int64_t first_pts;          /* pts of the first presented frame since (re)start (AV_NOPTS_VALUE) */
    uint64_t start_ms;          /* wall clock at first_pts */
    int64_t cur_pts;            /* pts of the most recent presented frame (for position) */
    int have_frame;             /* c->yuv holds a decoded frame (safe to re-encode while paused) */
    uint64_t paused_next_ms;    /* pacing tick for the paused re-encode */
    volatile int paused;
    volatile int seek_pending;  /* a seek was requested from another thread */
    double seek_frac;
};

void bsdr_capture_set_overlay(bsdr_capture *c, struct bsdr_overlay *ov) {
    c->overlay = ov;
}

static int open_encoder(bsdr_capture *c, const bsdr_capture_config *cfg,
                        int w, int h) {
    const char *try_names[6];
    int n = 0;
    if (cfg->encoder) try_names[n++] = cfg->encoder;
#ifdef _WIN32
    /* NVIDIA -> AMD -> Intel -> Media Foundation -> software */
    else { try_names[n++] = "h264_nvenc"; try_names[n++] = "h264_amf";
           try_names[n++] = "h264_qsv";   try_names[n++] = "h264_mf";
           try_names[n++] = "libx264"; }
#else
    else { try_names[n++] = "h264_nvenc"; try_names[n++] = "libx264"; }
#endif
    try_names[n] = NULL;

    for (int i = 0; i < n; i++) {
        const AVCodec *codec = avcodec_find_encoder_by_name(try_names[i]);
        if (!codec) continue;
        AVCodecContext *enc = avcodec_alloc_context3(codec);
        if (!enc) continue;
        enc->width = w;
        enc->height = h;
        enc->pix_fmt = AV_PIX_FMT_NV12;
        enc->time_base = (AVRational){ 1, cfg->fps };
        enc->framerate = (AVRational){ cfg->fps, 1 };
        enc->bit_rate = cfg->bitrate;
        enc->gop_size = cfg->fps;        /* ~1s keyframe interval */
        enc->max_b_frames = 0;           /* no B-frames -> low latency (High profile w/o B-frames) */
        /* HIGH profile, matching the official Bigscreen host (its SPS is profile_idc 100 on both LAN
         * and cloud). bsdrX previously used Constrained Baseline (66); the Quest's cloud consumer is
         * set up for the producer's negotiated High profile, so a Baseline stream made it choke. */
        enc->profile = AV_PROFILE_H264_HIGH;
        /* in-band SPS/PPS (no GLOBAL_HEADER) so late joiners can decode */
        if (strstr(try_names[i], "nvenc")) {
            /* Quality-tuned low-latency NVENC. The old preset=p4 + tune=ull (ultra-low latency)
             * combo shrinks the VBV to ~1 frame and disables adaptive quant, so at low bitrates
             * (e.g. 3 Mbps @ 1080p) complex desktop frames were heavily quantized and looked worse
             * than the x264 fallback. We keep latency low (tune=ll, no B-frames, no lookahead) but
             * restore the quality knobs: a slower preset, spatial AQ (the single biggest low-bitrate
             * win, no latency cost), and a ~1s CBR VBV so AQ can redistribute bits. p6 stays well
             * above real-time for 1080p30 on any NVENC GPU. */
            av_opt_set(enc->priv_data, "preset", "p7", 0);    /* max-quality preset; still real-time */
            av_opt_set(enc->priv_data, "profile", "high", 0);
            av_opt_set(enc->priv_data, "rc", "cbr", 0);
            av_opt_set_int(enc->priv_data, "spatial-aq", 1, 0);
            av_opt_set_int(enc->priv_data, "aq-strength", 8, 0);
            /* 2-pass rate control ("multipass"): the biggest quality-per-bit win at the Quest's low
             * bitrates — allocates bits far better than 1-pass CBR, for a negligible latency cost. */
            av_opt_set(enc->priv_data, "multipass", "fullres", 0);
            enc->rc_max_rate = cfg->bitrate;
            if (cfg->input_file) {
                /* File playback has NO latency constraint -> go for maximum quality: HQ tune,
                 * look-ahead, temporal AQ and B-frames (all of which the low-latency desktop path
                 * must avoid). A ~2s VBV lets complex scenes borrow bits. */
                av_opt_set(enc->priv_data, "tune", "hq", 0);
                av_opt_set_int(enc->priv_data, "rc-lookahead", 20, 0);
                av_opt_set_int(enc->priv_data, "temporal-aq", 1, 0);
                enc->max_b_frames = 3;
                enc->rc_buffer_size = cfg->bitrate * 2;
            } else {
                av_opt_set(enc->priv_data, "tune", "ll", 0);  /* low latency (no lookahead/B-frames) */
                enc->rc_buffer_size = cfg->bitrate;           /* ~1s VBV */
            }
        } else {
            /* 'veryfast' (not 'ultrafast'): ultrafast disables CABAC + 8x8dct, forcing the SPS to
             * Constrained Baseline (profile_idc 66) regardless of the requested profile — and the
             * Quest chokes on non-High streams. veryfast keeps the High-profile tools (idc 100),
             * stays real-time, and looks better. zerolatency keeps it low-latency (no lookahead/B). */
            av_opt_set(enc->priv_data, "preset", "veryfast", 0);
            av_opt_set(enc->priv_data, "tune", "zerolatency", 0);
            av_opt_set(enc->priv_data, "profile", "high", 0);
            /* SINGLE SLICE PER FRAME — critical for the Quest. The Bigscreen wire format keys frame
             * reassembly on (sessid, frame_id) and gives every NAL its own frame_id, so a picture
             * MUST be one slice/NAL. But tune=zerolatency turns on sliced-threads, which splits each
             * frame into (thread-count) slice-NALs — e.g. 8 on an 8-core box. The headset then reads
             * 8 "frames" per picture and freezes. (This is exactly why the CPU/3D path froze while
             * nvenc, at 1 slice, worked.) Live desktop -> single thread (single slice + zero latency);
             * a file tolerates latency, so keep multi-core FRAME threading but disable slicing. */
            if (cfg->input_file)
                av_opt_set(enc->priv_data, "x264-params", "sliced-threads=0", 0);
            else
                enc->thread_count = 1;
            /* NO VBV cap on x264. It only ever runs when the operator chose the software path
             * (--cpu) or on a GPU-less box, where quality is the whole point — a max-rate/buffer
             * makes --cpu look worse. x264's larger IDRs don't freeze the Quest (that was the
             * multi-slice bug, fixed above; nvenc's IDRs are bigger and work). The nvenc branch keeps
             * its own VBV; on a GPU box 3D encodes with nvenc, so this path is genuinely CPU-only. */
        }
        if (avcodec_open2(enc, codec, NULL) == 0) {
            c->enc = enc;
            c->enc_name = try_names[i];
            BSDR_INFO("bsdr.capture", "encoder %s %dx%d @%dfps %dbps (High)",
                      try_names[i], w, h, cfg->fps, cfg->bitrate);
            return 0;
        }
        BSDR_WARN("bsdr.capture", "encoder %s failed to open, trying next", try_names[i]);
        avcodec_free_context(&enc);
    }
    return -1;
}

/* Open h264_nvenc taking CUDA frames (NV12) from the filter graph's hw_frames_ctx. */
static int open_encoder_cuda(bsdr_capture *c, const bsdr_capture_config *cfg,
                             int w, int h, AVBufferRef *hw_frames) {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) return -1;
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    if (!enc) return -1;
    enc->width = w; enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_CUDA;
    /* sw format = whatever the CUDA frames carry (bgr0 from x11grab); nvenc converts to NV12 itself. */
    enc->sw_pix_fmt = ((AVHWFramesContext *)hw_frames->data)->sw_format;
    enc->hw_frames_ctx = av_buffer_ref(hw_frames);
    enc->time_base = (AVRational){ 1, cfg->fps };
    enc->framerate = (AVRational){ cfg->fps, 1 };
    enc->bit_rate = cfg->bitrate;
    enc->gop_size = cfg->fps;
    enc->max_b_frames = 0;
    enc->profile = AV_PROFILE_H264_HIGH;
    /* Same quality-tuned low-latency NVENC config as the NV12 path (see the note there): p6 + ll +
     * spatial AQ + a ~1s CBR VBV, instead of p4 + ull which looked worse than x264 at low bitrate. */
    av_opt_set(enc->priv_data, "preset", "p7", 0);   /* max-quality preset; still real-time */
    av_opt_set(enc->priv_data, "tune", "ll", 0);
    av_opt_set(enc->priv_data, "profile", "high", 0);
    av_opt_set(enc->priv_data, "rc", "cbr", 0);
    av_opt_set_int(enc->priv_data, "spatial-aq", 1, 0);
    av_opt_set_int(enc->priv_data, "aq-strength", 8, 0);
    av_opt_set(enc->priv_data, "multipass", "fullres", 0);   /* 2-pass RC: big low-bitrate quality win */
    enc->rc_max_rate = cfg->bitrate;
    enc->rc_buffer_size = cfg->bitrate;
    if (avcodec_open2(enc, codec, NULL) != 0) { avcodec_free_context(&enc); return -1; }
    c->enc = enc; c->enc_name = "h264_nvenc(cuda)";
    return 0;
}

/* Tear down any partially-built GPU state (so we can fall back to the CPU path cleanly). */
static void gpu_teardown(bsdr_capture *c) {
    if (c->enc) avcodec_free_context(&c->enc);
    if (c->fg) avfilter_graph_free(&c->fg);   /* frees all filters incl. their device refs */
    if (c->gpu) av_frame_free(&c->gpu);
    if (c->hw_device) av_buffer_unref(&c->hw_device);
    c->fg_src = c->fg_sink = NULL;
    c->enc_name = NULL;
    c->use_gpu = 0;
}

/* Build the CUDA pipeline: buffer(BGRA) -> hwupload_cuda -> scale_cuda(NV12) -> buffersink, and open
 * nvenc on the resulting CUDA frames. Validated end-to-end with a dummy frame so a build whose
 * scale_cuda can't take the x11grab format falls back to CPU cleanly at init. Returns 0 on success. */
static int setup_gpu(bsdr_capture *c, const bsdr_capture_config *cfg, int ow, int oh) {
    const char *ifmt = av_get_pix_fmt_name(c->dec->pix_fmt);
    if (av_hwdevice_ctx_create(&c->hw_device, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0) < 0) {
        BSDR_WARN("bsdr.capture", "GPU: no CUDA device (driver/libcuda? launch under prime-run on Optimus)");
        return -1;
    }
    c->fg = avfilter_graph_alloc();
    if (!c->fg) return -1;
    AVRational tb = c->fmt->streams[0]->time_base;
    char args[256];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             c->dec->width, c->dec->height, (int)c->dec->pix_fmt,
             tb.num ? tb.num : 1, tb.den ? tb.den : cfg->fps);
    const AVFilter *fbuf = avfilter_get_by_name("buffer"), *fsink = avfilter_get_by_name("buffersink");
    const AVFilter *fup = avfilter_get_by_name("hwupload_cuda"), *fsc = avfilter_get_by_name("scale_cuda");
    if (!fbuf || !fsink || !fup || !fsc) {
        BSDR_WARN("bsdr.capture", "GPU: hwupload_cuda/scale_cuda filters missing in this ffmpeg build");
        return -1;
    }
    if (avfilter_graph_create_filter(&c->fg_src, fbuf, "in", args, NULL, c->fg) < 0) return -1;
    AVFilterContext *hwup = avfilter_graph_alloc_filter(c->fg, fup, "hwup");
    if (!hwup) return -1;
    hwup->hw_device_ctx = av_buffer_ref(c->hw_device);
    if (avfilter_init_str(hwup, NULL) < 0) return -1;
    /* scale_cuda is YUV-only for FORMAT conversion (bgr0->nv12 fails: "Unsupported conversion"), but it
     * RESIZES bgr0 fine — so keep the captured format through the resize and let nvenc do bgr0->nv12 on
     * the GPU (nvenc accepts CUDA bgr0/rgb0 frames). Full GPU offload without a CPU color convert. */
    char sargs[64]; snprintf(sargs, sizeof(sargs), "%d:%d", ow, oh);
    AVFilterContext *scale = NULL;
    if (avfilter_graph_create_filter(&scale, fsc, "scale", sargs, NULL, c->fg) < 0) return -1;
    if (avfilter_graph_create_filter(&c->fg_sink, fsink, "out", NULL, NULL, c->fg) < 0) return -1;
    if (avfilter_link(c->fg_src, 0, hwup, 0) < 0) return -1;
    if (avfilter_link(hwup, 0, scale, 0) < 0) return -1;
    if (avfilter_link(scale, 0, c->fg_sink, 0) < 0) return -1;
    if (avfilter_graph_config(c->fg, NULL) < 0) return -1;
    AVBufferRef *frames = av_buffersink_get_hw_frames_ctx(c->fg_sink);
    if (!frames || open_encoder_cuda(c, cfg, ow, oh, frames) < 0) return -1;
    /* validate: push one dummy frame through upload+scale on the GPU */
    AVFrame *t = av_frame_alloc(); if (!t) return -1;
    t->format = c->dec->pix_fmt; t->width = c->dec->width; t->height = c->dec->height;
    int ok = (av_frame_get_buffer(t, 32) == 0);
    if (ok) { t->pts = 0; ok = (av_buffersrc_add_frame(c->fg_src, t) == 0); }
    if (ok) { AVFrame *o = av_frame_alloc();
              ok = (o && av_buffersink_get_frame(c->fg_sink, o) == 0);
              if (o) av_frame_free(&o); }
    av_frame_free(&t);
    if (!ok) {
        BSDR_WARN("bsdr.capture", "GPU: validation failed (input %s through hwupload+scale_cuda)",
                  ifmt ? ifmt : "?");
        return -1;
    }
    c->gpu = av_frame_alloc();
    if (!c->gpu) return -1;
    BSDR_INFO("bsdr.capture",
              "encoder h264_nvenc %dx%d @%dfps %dbps (High, GPU: %s->scale_cuda->nvenc nv12)",
              ow, oh, cfg->fps, cfg->bitrate, ifmt ? ifmt : "?");
    return 0;
}

/* Open h264_vaapi taking VAAPI surfaces (NV12) from the filter graph's hw_frames_ctx. */
static int open_encoder_vaapi(bsdr_capture *c, const bsdr_capture_config *cfg,
                              int w, int h, AVBufferRef *hw_frames) {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) return -1;
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    if (!enc) return -1;
    enc->width = w; enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_VAAPI;
    enc->hw_frames_ctx = av_buffer_ref(hw_frames);
    enc->time_base = (AVRational){ 1, cfg->fps };
    enc->framerate = (AVRational){ cfg->fps, 1 };
    enc->bit_rate = cfg->bitrate;
    enc->gop_size = cfg->fps;
    enc->max_b_frames = 0;
    enc->profile = AV_PROFILE_H264_HIGH;
    enc->rc_max_rate = cfg->bitrate;
    enc->rc_buffer_size = cfg->bitrate;   /* ~1s VBV: lets CBR spend bits on complex frames (quality) */
    av_opt_set(enc->priv_data, "rc_mode", "CBR", 0);
    av_opt_set_int(enc->priv_data, "low_power", 1, 0);   /* VCN low-latency path; ignored if unsupported */
    if (avcodec_open2(enc, codec, NULL) != 0) {
        av_opt_set_int(enc->priv_data, "low_power", 0, 0);
        if (avcodec_open2(enc, codec, NULL) != 0) { avcodec_free_context(&enc); return -1; }
    }
    c->enc = enc; c->enc_name = "h264_vaapi";
    return 0;
}

/* Build a VAAPI pipeline on the iGPU and open h264_vaapi. Two inputs:
 *  - x11grab (bgr0 sw frames): buffer -> hwupload -> scale_vaapi(nv12) -> buffersink
 *  - kmsgrab  (drm_prime hw frames): buffer -> hwmap(derive=vaapi) -> scale_vaapi(nv12) -> buffersink
 * VAAPI VPP (scale_vaapi) CAN do RGB->NV12 (unlike scale_cuda), so x11grab works directly. AMD needs
 * the radeonsi VA driver; default it if the user hasn't. Validated with a dummy frame -> CPU fallback
 * on any failure. */
static int setup_vaapi(bsdr_capture *c, const bsdr_capture_config *cfg, int ow, int oh) {
#if !defined(_WIN32)
    setenv("LIBVA_DRIVER_NAME", "radeonsi", 0);   /* AMD VCN; respects an existing value (Linux VAAPI only) */
#endif
    int hw_in = (c->dec->hw_frames_ctx != NULL) || (c->dec->pix_fmt == AV_PIX_FMT_DRM_PRIME);
    if (av_hwdevice_ctx_create(&c->hw_device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0) < 0) {
        BSDR_WARN("bsdr.capture", "VAAPI: no device on /dev/dri/renderD128 (install mesa-va-drivers?)");
        return -1;
    }
    c->fg = avfilter_graph_alloc();
    if (!c->fg) return -1;
    AVRational tb = c->fmt->streams[0]->time_base;
    char args[256];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             c->dec->width, c->dec->height, (int)c->dec->pix_fmt,
             tb.num ? tb.num : 1, tb.den ? tb.den : cfg->fps);
    const AVFilter *fbuf = avfilter_get_by_name("buffer"), *fsink = avfilter_get_by_name("buffersink");
    const AVFilter *fmap = avfilter_get_by_name(hw_in ? "hwmap" : "hwupload");
    const AVFilter *fsc  = avfilter_get_by_name("scale_vaapi");
    if (!fbuf || !fsink || !fmap || !fsc) { BSDR_WARN("bsdr.capture", "VAAPI: filters missing"); return -1; }
    if (avfilter_graph_create_filter(&c->fg_src, fbuf, "in", args, NULL, c->fg) < 0) return -1;
    if (hw_in) {   /* kmsgrab: the source frames carry their own hw_frames_ctx */
        AVBufferSrcParameters *p = av_buffersrc_parameters_alloc();
        if (p) { p->hw_frames_ctx = c->dec->hw_frames_ctx; av_buffersrc_parameters_set(c->fg_src, p); av_free(p); }
    }
    AVFilterContext *map = avfilter_graph_alloc_filter(c->fg, fmap, "map");
    if (!map) return -1;
    if (hw_in) av_opt_set(map, "derive_device", "vaapi", AV_OPT_SEARCH_CHILDREN);
    map->hw_device_ctx = av_buffer_ref(c->hw_device);
    if (avfilter_init_str(map, NULL) < 0) return -1;
    char sargs[64]; snprintf(sargs, sizeof(sargs), "%d:%d:format=nv12", ow, oh);
    AVFilterContext *scale = NULL;
    if (avfilter_graph_create_filter(&scale, fsc, "scale", sargs, NULL, c->fg) < 0) return -1;
    if (avfilter_graph_create_filter(&c->fg_sink, fsink, "out", NULL, NULL, c->fg) < 0) return -1;
    if (avfilter_link(c->fg_src, 0, map, 0) < 0) return -1;
    if (avfilter_link(map, 0, scale, 0) < 0) return -1;
    if (avfilter_link(scale, 0, c->fg_sink, 0) < 0) return -1;
    if (avfilter_graph_config(c->fg, NULL) < 0) { BSDR_WARN("bsdr.capture", "VAAPI: graph config failed"); return -1; }
    AVBufferRef *frames = av_buffersink_get_hw_frames_ctx(c->fg_sink);
    if (!frames || open_encoder_vaapi(c, cfg, ow, oh, frames) < 0) { BSDR_WARN("bsdr.capture", "VAAPI: h264_vaapi open failed"); return -1; }
    c->gpu = av_frame_alloc();
    if (!c->gpu) return -1;
    BSDR_INFO("bsdr.capture", "encoder h264_vaapi %dx%d @%dfps %dbps (High, iGPU: %s->scale_vaapi->vaapi nv12)",
              ow, oh, cfg->fps, cfg->bitrate, hw_in ? "kmsgrab" : "x11grab");
    return 0;
}

bsdr_capture *bsdr_capture_open(const bsdr_capture_config *cfg_in) {
    bsdr_capture_config cfg = *cfg_in;
    if (!cfg.display) cfg.display = getenv("DISPLAY") ? getenv("DISPLAY") : ":0";
    if (cfg.fps <= 0) cfg.fps = 30;
    if (cfg.bitrate <= 0) cfg.bitrate = 8000000;

    avdevice_register_all();
    bsdr_capture *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fps = cfg.fps;
    c->first_pts = AV_NOPTS_VALUE;

    /* ---- File source: decode a media file instead of grabbing the screen. Re-encoding lets us
     * composite the in-VR media bar; the overlay only draws on the CPU scale path, so force it. */
    if (cfg.input_file) {
        c->is_file = 1;
        c->loop = cfg.loop;
        cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0;
        /* The path is UNTRUSTED (web UI / playlist). Local files get a tight local-only protocol set
         * (no SSRF, no concat/pipe arbitrary-file reads). An explicit http/https/rtsp URL — validated
         * upstream by bsdr_url_scheme_ok — additionally gets the network transports so the operator
         * can stream a web/RTSP source. Other schemes never reach libavformat. */
        int is_url = strstr(cfg.input_file, "://") != NULL;
        AVDictionary *fopts = NULL;
        av_dict_set(&fopts, "protocol_whitelist",
                   is_url ? "file,crypto,subfile,data,http,https,tls,tcp,rtp,rtsp,udp,hls"
                          : "file,crypto,subfile,data", 0);
        int frc = avformat_open_input(&c->fmt, cfg.input_file, NULL, &fopts);
        av_dict_free(&fopts);
        if (frc != 0) {
            BSDR_ERROR("bsdr.capture", "cannot open file %s", cfg.input_file);
            bsdr_capture_close(c); return NULL;
        }
        goto have_input;
    }

    AVDictionary *opts = NULL;
    char vsize[32];
    if (cfg.width > 0 && cfg.height > 0) {
        snprintf(vsize, sizeof(vsize), "%dx%d", cfg.width, cfg.height);
        av_dict_set(&opts, "video_size", vsize, 0);
    }
    char fr[16]; snprintf(fr, sizeof(fr), "%d", cfg.fps);
    av_dict_set(&opts, "framerate", fr, 0);
    av_dict_set(&opts, "draw_mouse", "1", 0);

#ifdef _WIN32
    /* Windows: GDI desktop grab. Region via offset_x/offset_y + video_size
     * (the web UI already turns a window selection into an x/y/w/h region). */
    const AVInputFormat *ifmt = av_find_input_format("gdigrab");
    if (!ifmt) { BSDR_ERROR("bsdr.capture", "gdigrab not available"); av_dict_free(&opts); goto fail; }
    if (cfg.x || cfg.y) {
        char ox[16], oy[16];
        snprintf(ox, sizeof(ox), "%d", cfg.x);
        snprintf(oy, sizeof(oy), "%d", cfg.y);
        av_dict_set(&opts, "offset_x", ox, 0);
        av_dict_set(&opts, "offset_y", oy, 0);
    }
    const char *url = "desktop";
    if (avformat_open_input(&c->fmt, url, ifmt, &opts) != 0) {
        BSDR_ERROR("bsdr.capture", "cannot open gdigrab desktop");
        av_dict_free(&opts); goto fail;
    }
#else
    if (cfg.use_kmsgrab) cfg.use_vaapi = 1;   /* kmsgrab frames are DRM hw surfaces -> need the VAAPI path */
    char url[64];
    const AVInputFormat *ifmt;
    if (cfg.use_kmsgrab) {
        /* DRM/KMS capture: zero-copy, whole-CRTC (the region is cropped later by scale_vaapi). Needs
         * CAP_SYS_ADMIN on the binary (setcap cap_sys_admin+ep build/bsdr_agent) or root. card0 = the
         * GPU driving the display (AMD here). Falls back to x11grab if it can't open. */
        ifmt = av_find_input_format("kmsgrab");
        if (ifmt) {
            av_dict_set(&opts, "device", "/dev/dri/card0", 0);
            av_dict_set(&opts, "framerate", fr, 0);
            if (avformat_open_input(&c->fmt, "", ifmt, &opts) == 0) goto input_ok;
            BSDR_WARN("bsdr.capture", "kmsgrab failed (need CAP_SYS_ADMIN? setcap cap_sys_admin+ep) -> x11grab");
            av_dict_free(&opts); opts = NULL;
            av_dict_set(&opts, "framerate", fr, 0); av_dict_set(&opts, "draw_mouse", "1", 0);
            if (cfg.width > 0 && cfg.height > 0) av_dict_set(&opts, "video_size", vsize, 0);
            cfg.use_kmsgrab = 0;
        }
    }
    ifmt = av_find_input_format("x11grab");
    if (!ifmt) { BSDR_ERROR("bsdr.capture", "x11grab not available"); av_dict_free(&opts); goto fail; }
    snprintf(url, sizeof(url), "%s+%d,%d", cfg.display, cfg.x, cfg.y);
    if (avformat_open_input(&c->fmt, url, ifmt, &opts) != 0) {
        BSDR_ERROR("bsdr.capture", "cannot open X display %s", url);
        av_dict_free(&opts); goto fail;
    }
input_ok:;
#endif
    av_dict_free(&opts);
have_input:;
    if (avformat_find_stream_info(c->fmt, NULL) < 0) goto fail;
    if (c->is_file) {
        c->vstream = av_find_best_stream(c->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (c->vstream < 0) { BSDR_ERROR("bsdr.capture", "no video stream in file"); goto fail; }
        AVStream *vst = c->fmt->streams[c->vstream];
        c->file_tb = vst->time_base;
        c->duration_s = c->fmt->duration > 0 ? (double)c->fmt->duration / AV_TIME_BASE : 0.0;
        int ffps = vst->avg_frame_rate.num ? (int)(av_q2d(vst->avg_frame_rate) + 0.5) : cfg.fps;
        if (ffps > 0) { c->fps = ffps; cfg.fps = ffps; }   /* keep encoder + rtp clock at the file rate */
    } else {
        c->vstream = 0;
    }
    AVCodecParameters *par = c->fmt->streams[c->vstream]->codecpar;

    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { BSDR_ERROR("bsdr.capture", "no decoder for codec_id %d", par->codec_id); goto fail; }
    c->dec = avcodec_alloc_context3(dec);
    if (!c->dec) goto fail;
    avcodec_parameters_to_context(c->dec, par);
    if (avcodec_open2(c->dec, dec, NULL) < 0) goto fail;

    /* Target dims: 0 means "derive from source". If only one is given, scale the
     * other to preserve the desktop's aspect ratio (so e.g. 720p on a 16:10 or
     * ultrawide desktop is not stretched). The Quest sizes its surface from the
     * encoded SPS, so the aspect ratio here is what the headset displays. */
    int ow = cfg.out_width, oh = cfg.out_height;
    if (ow <= 0 && oh <= 0) { ow = c->dec->width; oh = c->dec->height; }
    else if (ow <= 0)       ow = (int)((long)c->dec->width * oh / c->dec->height);
    else if (oh <= 0)       oh = (int)((long)c->dec->height * ow / c->dec->width);
    ow &= ~1; oh &= ~1;   /* even dimensions for 4:2:0 */
    /* 2D->3D "full resolution per eye": render the half-SBS frame at 2x linear resolution, keeping
     * the SCREEN ASPECT (a 2x-wide frame would just make the Quest's screen double-wide, not 3D). At
     * 2x both dims each squished eye holds ~2x the horizontal detail — but it's ~4x the pixels, so
     * also raise the bitrate (2x) so those pixels aren't starved into mush. Heavy path (opt-in). */
    if (cfg.threed_mode && cfg.threed_full) {
        if ((long)ow * 2 <= 3840) {
            ow *= 2; oh *= 2; ow &= ~1; oh &= ~1;
            long br = (long)cfg.bitrate * 2;
            if (br > 24000000) br = 24000000;     /* cap so weak Wi-Fi isn't flooded */
            if (br > cfg.bitrate) cfg.bitrate = (int)br;
        }
    }
    c->raw = av_frame_alloc();
    c->ipkt = av_packet_alloc();
    c->opkt = av_packet_alloc();
    if (!c->raw || !c->ipkt || !c->opkt) goto fail;

    /* The SBS is synthesised on a CPU NV12 frame, so 3D always uses the CPU-scale path — the
     * VAAPI/CUDA branches below are skipped for it. The packed output is the same size as the source
     * (half-SBS), so the encoder is sized to ow x oh. */
    int enc_w = ow, enc_h = oh;
    if (cfg.threed_mode) {
        bsdr_threed_config tc = { .mode = cfg.threed_mode, .deepness = cfg.threed_deepness,
                                  .convergence = cfg.threed_convergence, .swap = cfg.threed_swap };
        snprintf(tc.ai_cmd, sizeof tc.ai_cmd, "%s", cfg.threed_ai_cmd);
        c->threed = bsdr_threed_create(ow, oh, &tc);
        if (c->threed) enc_w = bsdr_threed_out_width(c->threed);
    }

    /* Encode pipeline, in preference order, each falling back to the next:
     *  --vaapi  -> iGPU VAAPI (frees the dGPU; the only path that pairs with --kmsgrab)
     *  default  -> CUDA/NVENC GPU (scale/convert off the CPU)
     *  --cpu / any failure / 3D -> CPU sws_scale + nvenc/x264 */
    if (cfg.use_vaapi && !c->threed) {
        if (setup_vaapi(c, &cfg, ow, oh) == 0) { c->use_gpu = 1; return c; }
        gpu_teardown(c);
        BSDR_WARN("bsdr.capture", "VAAPI pipeline unavailable -> CPU scale/convert");
    } else if (!cfg.cpu_only && !c->threed) {
        if (setup_gpu(c, &cfg, ow, oh) == 0) { c->use_gpu = 1; return c; }
        gpu_teardown(c);
        BSDR_WARN("bsdr.capture", "CUDA pipeline unavailable -> CPU scale/convert");
    }
    if (open_encoder(c, &cfg, enc_w, enc_h) < 0) {
        BSDR_ERROR("bsdr.capture", "no usable H.264 encoder"); goto fail;
    }
    c->sws = sws_getContext(c->dec->width, c->dec->height, c->dec->pix_fmt,
                            ow, oh, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
    c->yuv = av_frame_alloc();
    if (!c->sws || !c->yuv) { BSDR_ERROR("bsdr.capture", "sws/frame alloc failed"); goto fail; }
    c->yuv->format = AV_PIX_FMT_NV12; c->yuv->width = enc_w; c->yuv->height = enc_h;
    if (av_frame_get_buffer(c->yuv, 32) < 0) { BSDR_ERROR("bsdr.capture", "yuv buffer alloc failed"); goto fail; }
    if (c->threed) {
        /* the desktop is scaled into c->src (ow x oh); the SBS transform writes c->yuv (enc_w) */
        c->src = av_frame_alloc();
        if (!c->src) goto fail;
        c->src->format = AV_PIX_FMT_NV12; c->src->width = ow; c->src->height = oh;
        if (av_frame_get_buffer(c->src, 32) < 0) { BSDR_ERROR("bsdr.capture", "src buffer alloc failed"); goto fail; }
    }
    return c;
fail:
    bsdr_capture_close(c);
    return NULL;
}

/* ---- file-source playback helpers -------------------------------------------------------- */
static void cap_do_seek(bsdr_capture *c) {
    double frac = c->seek_frac;
    c->seek_pending = 0;
    if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
    if (c->duration_s <= 0) return;
    int64_t ts = (int64_t)(frac * c->duration_s / av_q2d(c->file_tb));
    if (av_seek_frame(c->fmt, c->vstream, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(c->dec);
        c->first_pts = AV_NOPTS_VALUE;   /* restart pacing from the new position */
    }
}

/* Sleep so this frame is presented at its PTS relative to the first (wall-clock pacing). */
static void cap_pace(bsdr_capture *c, int64_t pts) {
    if (pts == AV_NOPTS_VALUE) return;
    c->cur_pts = pts;
    if (c->first_pts == AV_NOPTS_VALUE) { c->first_pts = pts; c->start_ms = bsdr_now_ms(); return; }
    double rel = av_q2d(c->file_tb) * (double)(pts - c->first_pts);
    uint64_t target = c->start_ms + (uint64_t)(rel * 1000.0);
    uint64_t now = bsdr_now_ms();
    if (target > now && target - now < 2000) bsdr_sleep_ms((unsigned)(target - now));
}

/* Re-render the overlay onto the last decoded NV12 frame and encode it (paused = frozen video,
 * live stream + responsive bar). Returns 1 if a packet came out. */
static int cap_encode_yuv(bsdr_capture *c, const uint8_t **au, size_t *len, uint32_t *rtp_ts) {
    /* With 3D the overlay draws on the source (c->src), which the SBS then duplicates into both eyes;
     * otherwise it draws directly on the encoder frame. */
    AVFrame *pre = c->threed ? c->src : c->yuv;
    if (c->overlay)
        bsdr_overlay_render_nv12(c->overlay, pre->data[0], pre->linesize[0],
                                 pre->data[1], pre->linesize[1],
                                 pre->width, pre->height);
    if (c->threed)
        bsdr_threed_apply_nv12(c->threed, c->src->data[0], c->src->linesize[0],
                               c->src->data[1], c->src->linesize[1],
                               c->yuv->data[0], c->yuv->linesize[0],
                               c->yuv->data[1], c->yuv->linesize[1]);
    c->yuv->pts = c->frame_index;
    if (avcodec_send_frame(c->enc, c->yuv) < 0) return 0;
    if (avcodec_receive_packet(c->enc, c->opkt) == 0) {
        *au = c->opkt->data; *len = c->opkt->size;
        *rtp_ts = (uint32_t)(c->frame_index++ * BSDR_VIDEO_CLOCK_HZ / c->fps);
        return 1;
    }
    return 0;
}

int bsdr_capture_frame(bsdr_capture *c, const uint8_t **au, size_t *len,
                       uint32_t *rtp_ts) {
    /* pull already-encoded output first */
    int r = avcodec_receive_packet(c->enc, c->opkt);
    if (r == 0) {
        *au = c->opkt->data; *len = c->opkt->size;
        *rtp_ts = (uint32_t)(c->frame_index++ * BSDR_VIDEO_CLOCK_HZ / c->fps);
        return 1;
    }

    if (c->is_file) {
        if (c->seek_pending) cap_do_seek(c);
        if (c->paused) {                             /* freeze: re-encode the held frame at ~fps */
            if (!c->have_frame) { bsdr_sleep_ms(10); return 0; }
            int ms = 1000 / (c->fps > 0 ? c->fps : 30);
            uint64_t now = bsdr_now_ms();
            if (c->paused_next_ms > now && c->paused_next_ms - now < 500)
                bsdr_sleep_ms((unsigned)(c->paused_next_ms - now));
            c->paused_next_ms = bsdr_now_ms() + (unsigned)ms;
            return cap_encode_yuv(c, au, len, rtp_ts);
        }
    }

read_frame:
    /* grab one frame (screen or file), decode, scale, feed the encoder */
    if (av_read_frame(c->fmt, c->ipkt) < 0) {
        if (c->is_file && c->loop && av_seek_frame(c->fmt, c->vstream, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(c->dec); c->first_pts = AV_NOPTS_VALUE; goto read_frame;
        }
        return -1;
    }
    if (c->ipkt->stream_index != c->vstream) { av_packet_unref(c->ipkt); return 0; }
    if (avcodec_send_packet(c->dec, c->ipkt) < 0) { av_packet_unref(c->ipkt); return 0; }
    av_packet_unref(c->ipkt);
    if (avcodec_receive_frame(c->dec, c->raw) != 0) return 0;
    if (c->is_file) cap_pace(c, c->raw->pts);

    /* Voice-command balloon: composite onto the SOURCE frame (before scale/convert/
     * upload) so it survives both the CPU and the GPU (VAAPI/CUDA) encode paths — no
     * CPU-encode fallback needed. Only a software packed-RGB32 frame can be drawn on;
     * the zero-copy --kmsgrab DRM path has no CPU surface, so the balloon is skipped. */
    if (c->overlay && bsdr_overlay_balloon_on(c->overlay)) {
        int pf = c->raw->format;
        int bgr = (pf == AV_PIX_FMT_BGR0 || pf == AV_PIX_FMT_BGRA);
        int rgb = (pf == AV_PIX_FMT_RGB0 || pf == AV_PIX_FMT_RGBA);
        if ((bgr || rgb) && av_frame_make_writable(c->raw) == 0)
            bsdr_overlay_render_balloon(c->overlay, c->raw->data[0], c->raw->linesize[0],
                                        c->raw->width, c->raw->height, bgr, bsdr_now_ms());
        else {
            static int warned = 0;
            if (!warned) { warned = 1;
                BSDR_WARN("bsdr.capture", "balloon overlay skipped: source pixfmt %d not packed-RGB32 "
                          "(e.g. --kmsgrab zero-copy) — use x11grab/CPU or CUDA/VAAPI-from-x11grab", pf); }
        }
    }

    AVFrame *enc_in;
    if (c->use_gpu) {
        /* GPU: upload + scale + convert to NV12 on the CUDA device, feed nvenc a hw frame. The CPU
         * overlay can't touch a GPU surface, so the status overlay is CPU-path only. */
        c->raw->pts = c->frame_index;
        if (av_buffersrc_add_frame(c->fg_src, c->raw) < 0) { av_frame_unref(c->raw); return 0; }
        av_frame_unref(c->raw);
        av_frame_unref(c->gpu);
        if (av_buffersink_get_frame(c->fg_sink, c->gpu) < 0) return 0;
        c->gpu->pts = c->frame_index;
        enc_in = c->gpu;
    } else {
        /* Scale into the source buffer (c->src for 3D, else c->yuv directly), composite the overlay
         * on it, then for 3D synthesise the packed SBS frame into c->yuv. */
        AVFrame *pre = c->threed ? c->src : c->yuv;
        sws_scale(c->sws, (const uint8_t *const *)c->raw->data, c->raw->linesize,
                  0, c->dec->height, pre->data, pre->linesize);
        if (c->overlay)
            bsdr_overlay_render_nv12(c->overlay, pre->data[0], pre->linesize[0],
                                     pre->data[1], pre->linesize[1],
                                     pre->width, pre->height);
        if (c->threed)
            bsdr_threed_apply_nv12(c->threed, c->src->data[0], c->src->linesize[0],
                                   c->src->data[1], c->src->linesize[1],
                                   c->yuv->data[0], c->yuv->linesize[0],
                                   c->yuv->data[1], c->yuv->linesize[1]);
        c->yuv->pts = c->frame_index;   /* monotonic */
        c->have_frame = 1;              /* c->yuv/src now hold a frame we can re-encode while paused */
        av_frame_unref(c->raw);
        enc_in = c->yuv;
    }
    if (avcodec_send_frame(c->enc, enc_in) < 0) return 0;
    if (c->use_gpu) av_frame_unref(c->gpu);

    r = avcodec_receive_packet(c->enc, c->opkt);
    if (r == 0) {
        *au = c->opkt->data; *len = c->opkt->size;
        *rtp_ts = (uint32_t)(c->frame_index++ * BSDR_VIDEO_CLOCK_HZ / c->fps);
        return 1;
    }
    return 0;   /* encoder buffering; try again */
}

void bsdr_capture_info(bsdr_capture *c, int *w, int *h, const char **enc) {
    if (w) *w = c->enc ? c->enc->width : 0;
    if (h) *h = c->enc ? c->enc->height : 0;
    if (enc) *enc = c->enc_name;
}

/* ---- file-source playback controls (safe to call from another thread) -------------------- */
void bsdr_capture_seek(bsdr_capture *c, double frac) {
    if (!c || !c->is_file) return;
    c->seek_frac = frac;
    c->seek_pending = 1;   /* applied in bsdr_capture_frame, which owns fmt/dec */
}
void bsdr_capture_set_paused(bsdr_capture *c, int paused) {
    if (c && c->is_file) c->paused = paused ? 1 : 0;
}
int bsdr_capture_is_paused(bsdr_capture *c) {
    return (c && c->is_file) ? c->paused : 0;
}
double bsdr_capture_position(bsdr_capture *c, int *seekable) {
    if (!c || !c->is_file || c->duration_s <= 0) { if (seekable) *seekable = 0; return 0.0; }
    if (seekable) *seekable = 1;
    double t = av_q2d(c->file_tb) * (double)c->cur_pts;
    double f = t / c->duration_s;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

void bsdr_capture_close(bsdr_capture *c) {
    if (!c) return;
    if (c->opkt) { av_packet_unref(c->opkt); av_packet_free(&c->opkt); }
    if (c->ipkt) { av_packet_unref(c->ipkt); av_packet_free(&c->ipkt); }
    if (c->raw) av_frame_free(&c->raw);
    if (c->yuv) av_frame_free(&c->yuv);
    if (c->src) av_frame_free(&c->src);
    if (c->gpu) av_frame_free(&c->gpu);
    if (c->sws) sws_freeContext(c->sws);
    if (c->enc) avcodec_free_context(&c->enc);
    if (c->fg) avfilter_graph_free(&c->fg);
    if (c->hw_device) av_buffer_unref(&c->hw_device);
    if (c->dec) avcodec_free_context(&c->dec);
    if (c->fmt) avformat_close_input(&c->fmt);
    if (c->threed) bsdr_threed_close(c->threed);
    free(c);
}
