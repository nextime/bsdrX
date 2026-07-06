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
#include "bsdr/faceswap.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr/capture_pipewire.h"   /* Wayland desktop capture (portal + PipeWire); Linux only */

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

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
/* macOS gates screen capture behind the Screen Recording TCC permission (10.15+). Without it,
 * avfoundation silently hands back black/zero frames or fails to open. Preflight first (no prompt);
 * if we don't have it, trigger the one-time system dialog — same UX as Android's MediaProjection
 * grant. Returns 1 if access is (now) granted, 0 otherwise. Weak-linked so the binary still loads on
 * a pre-10.15 macOS, where capture just proceeds unchecked. */
extern bool CGPreflightScreenCaptureAccess(void) __attribute__((weak_import));
extern bool CGRequestScreenCaptureAccess(void)  __attribute__((weak_import));
static int macos_ensure_screen_capture_access(void) {
    if (!CGPreflightScreenCaptureAccess) return 1;     /* symbol absent (< 10.15): nothing to gate */
    if (CGPreflightScreenCaptureAccess()) return 1;    /* already granted */
    BSDR_WARN("bsdr.capture", "Screen Recording permission not granted — requesting it now "
              "(approve bsdrX in System Settings > Privacy & Security > Screen Recording, then retry)");
    int granted = CGRequestScreenCaptureAccess ? CGRequestScreenCaptureAccess() : 0;
    if (!granted)
        BSDR_ERROR("bsdr.capture", "Screen Recording permission denied; grant it in "
                   "System Settings > Privacy & Security > Screen Recording and restart bsdrX");
    return granted;
}
#endif

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
    /* ---- Wayland source (xdg-desktop-portal + PipeWire): frames arrive already-decoded (packed
     * 32-bit RGB), so we skip av_read_frame/decode and drop them straight into c->raw. fmt/dec are
     * lightweight dummies that only carry size + pix_fmt + time_base for the scale/encode setup. ---- */
    struct bsdr_pw_capture *pw;
    uint8_t *pw_buf;            /* packed BGR0/RGB0 frame (stride = pw_stride) filled each read */
    int pw_stride;
    /* ---- file-source playback (cfg.input_file) ---- */
    /* realtime face swap (borrowed engine, CPU path): NV12 <-> RGB24 scratch + scalers */
    struct bsdr_faceswap *faceswap;
    struct SwsContext *fs_to_rgb, *fs_from_rgb;
    uint8_t *fs_rgb;
    int is_file;                /* decoding a file rather than grabbing the screen */
    int is_webcam;              /* capturing a camera (live, like screen; CPU scale path) */
    int is_stereo;              /* two cameras composited side-by-side (real stereo SBS) */
    /* ---- stereo right-eye input (is_stereo): a second full decode+scale pipeline ---- */
    AVFormatContext *fmt2;
    AVCodecContext *dec2;
    struct SwsContext *sws2;
    AVFrame *raw2;
    int eye_w, eye_h;           /* per-eye scaled size; left/right written into c->yuv halves */
    int sbs_swap;               /* write left cam into the right half and vice-versa */
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

void bsdr_capture_set_faceswap(bsdr_capture *c, struct bsdr_faceswap *fs) {
    if (c) c->faceswap = fs;
}

int bsdr_capture_decode_image_rgb(const char *path, uint8_t **rgb, int *w, int *h) {
    if (!path || !rgb || !w || !h) return -1;
    AVFormatContext *fmt = NULL; int rc = -1;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) goto done;
    const AVCodec *dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) goto done;
    AVCodecContext *cc = avcodec_alloc_context3(dec);
    if (!cc) goto done;
    avcodec_parameters_to_context(cc, fmt->streams[vs]->codecpar);
    if (avcodec_open2(cc, dec, NULL) < 0) { avcodec_free_context(&cc); goto done; }
    AVPacket *pk = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
    while (av_read_frame(fmt, pk) >= 0) {
        if (pk->stream_index == vs && avcodec_send_packet(cc, pk) == 0 &&
            avcodec_receive_frame(cc, fr) == 0) { av_packet_unref(pk); break; }
        av_packet_unref(pk);
    }
    if (fr->width > 0) {
        int iw = fr->width, ih = fr->height;
        struct SwsContext *sw = sws_getContext(iw, ih, cc->pix_fmt, iw, ih, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
        uint8_t *buf = malloc((size_t)iw * ih * 3);
        if (sw && buf) {
            uint8_t *dst[1] = { buf }; int ds[1] = { iw*3 };
            sws_scale(sw, (const uint8_t *const *)fr->data, fr->linesize, 0, ih, dst, ds);
            *rgb = buf; *w = iw; *h = ih; rc = 0;
        } else free(buf);
        if (sw) sws_freeContext(sw);
    }
    av_frame_free(&fr); av_packet_free(&pk); avcodec_free_context(&cc);
done:
    avformat_close_input(&fmt);
    return rc;
}

/* Face swap on the pre-encode NV12 frame (CPU path): NV12 -> RGB24 -> swap in place -> NV12. Scalers
 * are built lazily at the frame size. A no-op until a source identity is set. */
static void apply_faceswap(bsdr_capture *c, AVFrame *f) {
    if (!c->faceswap || !bsdr_faceswap_ready(c->faceswap)) return;
    int w = f->width, h = f->height;
    if (!c->fs_rgb) {
        c->fs_to_rgb   = sws_getContext(w,h,AV_PIX_FMT_NV12, w,h,AV_PIX_FMT_RGB24, SWS_BILINEAR,NULL,NULL,NULL);
        c->fs_from_rgb = sws_getContext(w,h,AV_PIX_FMT_RGB24, w,h,AV_PIX_FMT_NV12, SWS_BILINEAR,NULL,NULL,NULL);
        c->fs_rgb = malloc((size_t)w*h*3);
        if (!c->fs_to_rgb || !c->fs_from_rgb || !c->fs_rgb) return;
    }
    uint8_t *rgb[1] = { c->fs_rgb }; int rs[1] = { w*3 };
    sws_scale(c->fs_to_rgb, (const uint8_t *const *)f->data, f->linesize, 0, h, rgb, rs);
    bsdr_faceswap_process_rgb(c->faceswap, c->fs_rgb, w, h);
    sws_scale(c->fs_from_rgb, (const uint8_t *const *)rgb, rs, 0, h, f->data, f->linesize);
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
#elif defined(__APPLE__)
    /* Apple VideoToolbox HW encode -> software x264 fallback */
    else { try_names[n++] = "h264_videotoolbox"; try_names[n++] = "libx264"; }
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
            /* Quality-tuned NVENC, matched to the x264 CPU fallback. History: preset=p4 + tune=ull
             * collapsed the VBV and killed adaptive quant, so desktop frames looked worse than x264.
             * The deeper reason the GPU path still trailed CPU at the SAME resolution+bitrate (e.g.
             * 1080p/3Mbps): the x264 branch runs with NO VBV cap ("quality is the whole point", see
             * that branch), so it overshoots the target on complex frames and stays sharp, while a
             * hard-capped nvenc holds the target and smears. We mirror x264 here — VBR toward a quality
             * target (cq) with GENEROUS HEADROOM (rc_max_rate = 2x the target) so nvenc spends the same
             * actual bits on busy frames: sharper text, less mush, at the same nominal bitrate. The
             * live path keeps latency low (tune=ll, no B-frames/lookahead); spatial AQ + 2-pass
             * concentrate bits on high-contrast text; p7 stays above real-time for 1080p30 on any
             * NVENC GPU. Busy-frame bandwidth rises to ~2x the target — exactly as the CPU path already
             * does — so cap it with --max-bitrate on a constrained (e.g. internet-share) uplink. */
            av_opt_set(enc->priv_data, "preset", "p7", 0);    /* max-quality preset; still real-time */
            av_opt_set(enc->priv_data, "profile", "high", 0);
            av_opt_set(enc->priv_data, "rc", "vbr", 0);       /* VBR (not CBR): don't pad static frames */
            av_opt_set_int(enc->priv_data, "spatial-aq", 1, 0);
            av_opt_set_int(enc->priv_data, "aq-strength", 8, 0);
            /* 2-pass rate control ("multipass"): the biggest quality-per-bit win at the Quest's low
             * bitrates — allocates bits far better than 1-pass, for a negligible latency cost. */
            av_opt_set(enc->priv_data, "multipass", "fullres", 0);
            if (cfg->input_file) {
                /* File playback has NO latency constraint -> go for maximum quality: HQ tune,
                 * look-ahead, temporal AQ and B-frames (all of which the low-latency desktop path
                 * must avoid). Bounded VBV is fine with no realtime deadline. */
                av_opt_set(enc->priv_data, "tune", "hq", 0);
                av_opt_set_int(enc->priv_data, "cq", 19, 0);
                av_opt_set_int(enc->priv_data, "rc-lookahead", 20, 0);
                av_opt_set_int(enc->priv_data, "temporal-aq", 1, 0);
                enc->max_b_frames = 3;
                enc->rc_max_rate = cfg->bitrate;
                enc->rc_buffer_size = (int64_t)cfg->bitrate * 2;
            } else {
                av_opt_set(enc->priv_data, "tune", "ll", 0);  /* low latency (no lookahead/B-frames) */
                av_opt_set_int(enc->priv_data, "cq", 20, 0);  /* quality target; maxrate bounds the spend */
                enc->rc_max_rate = (int64_t)cfg->bitrate * 2; /* 2x headroom = x264's practical overshoot */
                enc->rc_buffer_size = (int64_t)cfg->bitrate * 2;
            }
        } else if (strstr(try_names[i], "videotoolbox")) {
            /* Apple VideoToolbox HW H.264. It ignores the nvenc/x264 private options, so give it
             * its own: High profile to match the Quest consumer, realtime low-latency mode, and a
             * software fallback so a headless/VM macOS host still encodes. Single slice per frame is
             * VideoToolbox's default, so no thread_count fiddling (unlike the x264 branch). */
            av_opt_set(enc->priv_data, "profile", "high", 0);
            av_opt_set_int(enc->priv_data, "allow_sw", 1, 0);   /* fall back to SW if no HW encoder */
            if (!cfg->input_file) {
                av_opt_set_int(enc->priv_data, "realtime", 1, 0);
                av_opt_set_int(enc->priv_data, "prio_speed", 1, 0);
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
    /* Same quality-tuned low-latency NVENC config as the NV12 path (see the note there): p7 + ll +
     * spatial AQ + VBR with a cq target + 2x rc_max_rate HEADROOM, so on complex desktop frames nvenc
     * spends the same actual bits the uncapped x264 fallback does -> GPU quality matches CPU at the
     * same resolution+bitrate. (Was CBR, then hard-capped VBR, both of which held the target and
     * smeared while x264 overshot and stayed sharp.) --max-bitrate caps the target on a thin uplink. */
    av_opt_set(enc->priv_data, "preset", "p7", 0);   /* max-quality preset; still real-time */
    av_opt_set(enc->priv_data, "tune", "ll", 0);
    av_opt_set(enc->priv_data, "profile", "high", 0);
    av_opt_set(enc->priv_data, "rc", "vbr", 0);
    av_opt_set_int(enc->priv_data, "cq", 20, 0);
    av_opt_set_int(enc->priv_data, "spatial-aq", 1, 0);
    av_opt_set_int(enc->priv_data, "aq-strength", 8, 0);
    av_opt_set(enc->priv_data, "multipass", "fullres", 0);   /* 2-pass RC: big low-bitrate quality win */
    enc->rc_max_rate = (int64_t)cfg->bitrate * 2;    /* 2x headroom -> matches the uncapped x264 path */
    enc->rc_buffer_size = (int64_t)cfg->bitrate * 2;
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
    /* VBR with 2x headroom (was 1s CBR): CBR pads near-static desktop frames and a tight buffer caps
     * the IDR, leaving keyframe text soft while the uncapped x264 path overshoots and stays sharp.
     * VBR + rc_max_rate = 2x target lets VAAPI spend the same actual bits on complex frames (VBR also
     * genuinely needs maxrate > bitrate). Mirrors the NVENC paths; --max-bitrate caps a thin uplink. */
    enc->rc_max_rate = (int64_t)cfg->bitrate * 2;
    enc->rc_buffer_size = (int64_t)cfg->bitrate * 2;
    av_opt_set(enc->priv_data, "rc_mode", "VBR", 0);
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

/* Open one camera via the platform live-video input into *fmt. Shared by the single and stereo
 * paths. We don't pin resolution/framerate (a mismatch makes the device open fail); the camera's
 * native mode is taken and the scaler resizes to the output. Returns 0 on success. */
static int open_webcam(AVFormatContext **fmt, const char *dev) {
    const AVInputFormat *ifmt;
    AVDictionary *o = NULL;
    const char *url = dev;
#if defined(_WIN32)
    char buf[300];
    ifmt = av_find_input_format("dshow");
    if (strncmp(dev, "video=", 6) != 0) { snprintf(buf, sizeof buf, "video=%s", dev); url = buf; }
    av_dict_set(&o, "rtbufsize", "128M", 0);    /* absorb bursty USB webcam delivery */
#elif defined(__APPLE__)
    ifmt = av_find_input_format("avfoundation");  /* url = device index or name (cameras enum first) */
    av_dict_set(&o, "framerate", "30", 0);
#else
    ifmt = av_find_input_format("v4l2");
    if (!ifmt) ifmt = av_find_input_format("video4linux2");
#endif
    if (!ifmt) { BSDR_ERROR("bsdr.capture", "no webcam input backend on this platform"); av_dict_free(&o); return -1; }
    int rc = avformat_open_input(fmt, url, ifmt, &o);
    av_dict_free(&o);
    if (rc != 0) { BSDR_ERROR("bsdr.capture", "cannot open webcam '%s' (in use, or wrong device?)", dev); return -1; }
    return 0;
}

#ifdef BSDR_HAVE_PIPEWIRE
/* Open the Wayland portal + PipeWire screencast and set up lightweight dummy fmt/dec (size, pixfmt,
 * time_base) so the shared scale/encode setup and the read loop treat it like an x11grab source that
 * already decoded to packed 32-bit RGB. Returns 0 on success. */
static int cap_open_pipewire(bsdr_capture *c, bsdr_capture_config *cfg) {
    int w = 0, h = 0; bsdr_pw_format pf = BSDR_PW_FMT_BGR0;
    c->pw = bsdr_pw_capture_open(0 /*whole monitor*/, 1 /*cursor embedded*/, &w, &h, &pf);
    if (!c->pw || w <= 0 || h <= 0) return -1;
    enum AVPixelFormat avf =
        pf == BSDR_PW_FMT_RGB0 ? AV_PIX_FMT_RGB0 :
        pf == BSDR_PW_FMT_BGRA ? AV_PIX_FMT_BGRA :
        pf == BSDR_PW_FMT_RGBA ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGR0;
    c->fmt = avformat_alloc_context();
    if (!c->fmt) return -1;
    AVStream *st = avformat_new_stream(c->fmt, NULL);
    if (!st) return -1;
    st->time_base = (AVRational){ 1, cfg->fps };
    c->vstream = 0;
    c->dec = avcodec_alloc_context3(NULL);   /* not opened — only carries size/pixfmt for scale setup */
    if (!c->dec) return -1;
    c->dec->pix_fmt = avf;
    c->dec->width = w; c->dec->height = h;
    c->dec->time_base = (AVRational){ 1, cfg->fps };
    c->pw_stride = w * 4;                     /* packed 32-bit RGB, no row padding */
    c->pw_buf = malloc((size_t)c->pw_stride * h);
    if (!c->pw_buf) return -1;
    cfg->use_kmsgrab = 0;                     /* PipeWire delivers CPU frames (BGR0), like x11grab */
    BSDR_INFO("bsdr.capture", "Wayland capture via xdg-desktop-portal + PipeWire (%dx%d)", w, h);
    return 0;
}
#endif

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

    /* ---- Webcam source: capture a camera via the platform live-video input. Like the file source it
     * re-encodes on the CPU scale path so the overlay + 2D->3D compose. We deliberately DON'T pin the
     * camera's resolution/framerate (a mismatch makes the device open fail) — take the camera's native
     * mode and let the scaler resize to the output size. Stereo (webcam_right) is handled in
     * setup_stereo() further down; here we open the single/left camera. ---- */
    if (cfg.webcam) {
        c->is_webcam = 1;
        cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0;
        if (open_webcam(&c->fmt, cfg.webcam) != 0) { bsdr_capture_close(c); return NULL; }
        if (cfg.webcam_right && cfg.webcam_right[0]) {
            if (open_webcam(&c->fmt2, cfg.webcam_right) == 0) {
                c->is_stereo = 1;
                c->sbs_swap = cfg.threed_swap;
                cfg.threed_mode = 0;   /* real stereo composites two feeds -> bypass depth-based SBS */
            } else {
                BSDR_WARN("bsdr.capture", "stereo right camera '%s' failed -> single-camera 2D",
                          cfg.webcam_right);
            }
        }
        BSDR_INFO("bsdr.capture", "webcam source: %s%s", cfg.webcam,
                  c->is_stereo ? " + right eye (stereo SBS)" : "");
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
#elif defined(__APPLE__)
    /* macOS: avfoundation screen grab. It captures a whole display chosen by device index (screens
     * enumerate after cameras) and has NO grab-time region crop, so the web-UI x/y/w/h region is not
     * applied here — the CPU scale path downsizes the full display to the output size. cfg.display is
     * an X11 string on other platforms; reinterpret a bare number as a screen index, else default to
     * the first screen. draw_mouse/video_size (set above) are x11grab/gdigrab options; drop them and
     * use avfoundation's capture_cursor + the display's native size. */
    av_dict_set(&opts, "video_size", NULL, 0);   /* pass the native display size through; scale later */
    av_dict_set(&opts, "draw_mouse", NULL, 0);
    av_dict_set(&opts, "capture_cursor", "1", 0);
    cfg.cpu_only = 1;                             /* no VAAPI/CUDA on macOS -> CPU sws -> videotoolbox */
    macos_ensure_screen_capture_access();         /* guided one-time TCC prompt (like Android's grant) */
    const AVInputFormat *ifmt = av_find_input_format("avfoundation");
    if (!ifmt) { BSDR_ERROR("bsdr.capture", "avfoundation not available"); av_dict_free(&opts); goto fail; }
    char avdev[32];
    int scr = (cfg.display && cfg.display[0] >= '0' && cfg.display[0] <= '9') ? atoi(cfg.display) : 0;
    snprintf(avdev, sizeof(avdev), "Capture screen %d", scr);
    if (avformat_open_input(&c->fmt, avdev, ifmt, &opts) != 0) {
        BSDR_ERROR("bsdr.capture", "cannot open avfoundation \"%s\" (grant Screen Recording permission?)", avdev);
        av_dict_free(&opts); goto fail;
    }
#else
#ifdef BSDR_HAVE_PIPEWIRE
    /* Backend autodetect: x11grab can't see a native Wayland desktop (it grabs the XWayland root,
     * usually blank), so on a Wayland session use the xdg-desktop-portal + PipeWire path; on a real
     * Xorg session use x11grab. --x11 / --wayland force it. kmsgrab (explicit) always wins. */
    {
        int wayland = getenv("WAYLAND_DISPLAY") != NULL;
        int use_pw = cfg.force_pipewire ? 1 : (cfg.force_x11 || cfg.use_kmsgrab) ? 0 : wayland;
        if (use_pw) {
            if (cap_open_pipewire(c, &cfg) == 0) { av_dict_free(&opts); goto have_input_pw; }
            BSDR_WARN("bsdr.capture", "portal/PipeWire capture unavailable; falling back to x11grab");
        }
    }
#endif
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

    if (c->is_stereo) {   /* second camera: its own decode pipeline (scaled into the right eye) */
        if (avformat_find_stream_info(c->fmt2, NULL) < 0) { BSDR_ERROR("bsdr.capture", "stereo: no info from right cam"); goto fail; }
        AVCodecParameters *p2 = c->fmt2->streams[0]->codecpar;
        const AVCodec *d2 = avcodec_find_decoder(p2->codec_id);
        if (!d2) { BSDR_ERROR("bsdr.capture", "stereo: no decoder for right cam"); goto fail; }
        c->dec2 = avcodec_alloc_context3(d2);
        if (!c->dec2) goto fail;
        avcodec_parameters_to_context(c->dec2, p2);
        if (avcodec_open2(c->dec2, d2, NULL) < 0) { BSDR_ERROR("bsdr.capture", "stereo: right cam decoder open failed"); goto fail; }
        c->raw2 = av_frame_alloc();
        if (!c->raw2) goto fail;
    }

#ifdef BSDR_HAVE_PIPEWIRE
have_input_pw:;   /* PipeWire path joins here: fmt/dec already set by cap_open_pipewire (no avformat decode) */
#endif
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
    if (c->is_stereo) {
        /* Real stereo SBS: two eyes packed into one frame. Default = half-SBS (each eye squished to
         * ow/2, encoded frame stays ow wide, the format the Quest reads as SBS). threed_full = each
         * eye at full ow (encoded frame 2x wide), sharper but ~2x the pixels -> bump the bitrate. */
        if (cfg.threed_full && (long)ow * 2 <= 3840) {
            c->eye_w = ow & ~1; c->eye_h = oh & ~1;
            long br = (long)cfg.bitrate * 2; if (br > 24000000) br = 24000000;
            if (br > cfg.bitrate) cfg.bitrate = (int)br;
        } else {
            c->eye_w = (ow / 2) & ~1; c->eye_h = oh & ~1;
        }
        enc_w = c->eye_w * 2; enc_h = c->eye_h;
    } else if (cfg.threed_mode) {
        bsdr_threed_config tc = { .mode = cfg.threed_mode, .deepness = cfg.threed_deepness,
                                  .convergence = cfg.threed_convergence, .swap = cfg.threed_swap,
                                  .tier = cfg.threed_tier };
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
    if (c->is_stereo) {
        /* each camera scales to a per-eye NV12 tile (written into the two halves of c->yuv) */
        c->sws  = sws_getContext(c->dec->width,  c->dec->height,  c->dec->pix_fmt,
                                 c->eye_w, c->eye_h, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
        c->sws2 = sws_getContext(c->dec2->width, c->dec2->height, c->dec2->pix_fmt,
                                 c->eye_w, c->eye_h, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
        if (!c->sws2) { BSDR_ERROR("bsdr.capture", "stereo: right sws alloc failed"); goto fail; }
    } else {
        c->sws = sws_getContext(c->dec->width, c->dec->height, c->dec->pix_fmt,
                                ow, oh, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
    }
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
    apply_faceswap(c, c->yuv);
    if (avcodec_send_frame(c->enc, c->yuv) < 0) return 0;
    if (avcodec_receive_packet(c->enc, c->opkt) == 0) {
        *au = c->opkt->data; *len = c->opkt->size;
        *rtp_ts = (uint32_t)(c->frame_index++ * BSDR_VIDEO_CLOCK_HZ / c->fps);
        return 1;
    }
    return 0;
}

/* Stereo: read one decoded frame from (fmt,dec) and scale it into the c->yuv half at x-offset
 * dst_x (an NV12 sub-rectangle — Y and interleaved-UV both start at byte dst_x). Returns 1 on a
 * composed eye, 0 if no frame yet, <0 if the camera ended. */
static int read_scale_eye(bsdr_capture *c, AVFormatContext *fmt, AVCodecContext *dec,
                          AVFrame *raw, struct SwsContext *sws, int dst_x) {
    for (int tries = 0; tries < 256; tries++) {
        if (av_read_frame(fmt, c->ipkt) < 0) return -1;
        if (c->ipkt->stream_index != 0) { av_packet_unref(c->ipkt); continue; }
        int sr = avcodec_send_packet(dec, c->ipkt);
        av_packet_unref(c->ipkt);
        if (sr < 0) continue;
        if (avcodec_receive_frame(dec, raw) != 0) continue;
        uint8_t *dst[4] = { c->yuv->data[0] + dst_x, c->yuv->data[1] + dst_x, NULL, NULL };
        int dls[4] = { c->yuv->linesize[0], c->yuv->linesize[1], 0, 0 };
        sws_scale(sws, (const uint8_t *const *)raw->data, raw->linesize, 0, dec->height, dst, dls);
        av_frame_unref(raw);
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

    if (c->is_stereo) {
        /* compose one frame from BOTH cameras: left cam -> left half, right cam -> right half
         * (swapped if requested). Each eye is a scaled NV12 tile written into c->yuv. */
        int lx = c->sbs_swap ? c->eye_w : 0;
        int rx = c->sbs_swap ? 0 : c->eye_w;
        int a = read_scale_eye(c, c->fmt,  c->dec,  c->raw,  c->sws,  lx);
        int b = read_scale_eye(c, c->fmt2, c->dec2, c->raw2, c->sws2, rx);
        if (a < 0 || b < 0) return -1;
        if (a == 0 || b == 0) return 0;
        if (c->overlay)
            bsdr_overlay_render_nv12(c->overlay, c->yuv->data[0], c->yuv->linesize[0],
                                     c->yuv->data[1], c->yuv->linesize[1],
                                     c->yuv->width, c->yuv->height);
        c->yuv->pts = c->frame_index;
        c->have_frame = 1;
        apply_faceswap(c, c->yuv);
        if (avcodec_send_frame(c->enc, c->yuv) < 0) return 0;
        if (avcodec_receive_packet(c->enc, c->opkt) == 0) {
            *au = c->opkt->data; *len = c->opkt->size;
            *rtp_ts = (uint32_t)(c->frame_index++ * BSDR_VIDEO_CLOCK_HZ / c->fps);
            return 1;
        }
        return 0;
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

#ifdef BSDR_HAVE_PIPEWIRE
    if (c->pw) {
        /* Wayland: the portal/PipeWire stream already delivers packed 32-bit RGB — drop it into
         * c->raw (an unowned wrapper over pw_buf) and skip av_read_frame/decode entirely. */
        int got = bsdr_pw_capture_read(c->pw, c->pw_buf, c->pw_stride, c->dec->height);
        if (got < 0) return -1;
        if (got == 0) return 0;                 /* no new frame this tick — try again */
        av_frame_unref(c->raw);
        c->raw->format = c->dec->pix_fmt;
        c->raw->width  = c->dec->width;
        c->raw->height = c->dec->height;
        c->raw->data[0]     = c->pw_buf;
        c->raw->linesize[0] = c->pw_stride;
        goto have_raw;
    }
#endif
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

#ifdef BSDR_HAVE_PIPEWIRE
have_raw:;   /* jump target for the Wayland/PipeWire fast-path above; absent (and unreferenced) otherwise */
#endif
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
        apply_faceswap(c, c->yuv);      /* deepfake the encoder frame (CPU path only) */
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
#ifdef BSDR_HAVE_PIPEWIRE
    if (c->pw) {                              /* stop the PipeWire thread before freeing its scratch */
        bsdr_pw_capture_close(c->pw); c->pw = NULL;
        if (c->raw) { c->raw->data[0] = NULL; c->raw->linesize[0] = 0; }   /* was an unowned pw_buf wrapper */
        free(c->pw_buf); c->pw_buf = NULL;
        /* the dummy fmt was avformat_alloc_context()'d (never opened) -> free, don't close_input */
        if (c->fmt) { avformat_free_context(c->fmt); c->fmt = NULL; }
    }
#endif
    if (c->opkt) { av_packet_unref(c->opkt); av_packet_free(&c->opkt); }
    if (c->ipkt) { av_packet_unref(c->ipkt); av_packet_free(&c->ipkt); }
    if (c->raw) av_frame_free(&c->raw);
    if (c->raw2) av_frame_free(&c->raw2);
    if (c->yuv) av_frame_free(&c->yuv);
    if (c->src) av_frame_free(&c->src);
    if (c->gpu) av_frame_free(&c->gpu);
    if (c->sws) sws_freeContext(c->sws);
    if (c->sws2) sws_freeContext(c->sws2);
    if (c->fs_to_rgb) sws_freeContext(c->fs_to_rgb);
    if (c->fs_from_rgb) sws_freeContext(c->fs_from_rgb);
    free(c->fs_rgb);
    if (c->enc) avcodec_free_context(&c->enc);
    if (c->fg) avfilter_graph_free(&c->fg);
    if (c->hw_device) av_buffer_unref(&c->hw_device);
    if (c->dec) avcodec_free_context(&c->dec);
    if (c->dec2) avcodec_free_context(&c->dec2);
    if (c->fmt) avformat_close_input(&c->fmt);
    if (c->fmt2) avformat_close_input(&c->fmt2);
    if (c->threed) bsdr_threed_close(c->threed);
    free(c);
}
