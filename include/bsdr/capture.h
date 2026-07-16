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
/* Desktop capture + H.264 encode (Linux: X11 grab -> NVENC, fallback libx264).
 * Produces Annex-B H.264 access units to feed the RTP/SRTP video sender.
 * Constrained Baseline to match what the Quest expects (OpenH264). Built with
 * BSDR_ENABLE_VIDEO on Linux. */
#ifndef BSDR_CAPTURE_H
#define BSDR_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct bsdr_capture bsdr_capture;

typedef struct {
    const char *display;   /* X display, e.g. ":0.0" (NULL -> $DISPLAY or :0) */
    int x, y;              /* capture origin */
    int width, height;     /* 0 -> full screen */
    int out_width, out_height; /* 0 -> same as captured */
    int fps;               /* default 30 */
    int bitrate;           /* bps; default 8 Mbps */
    const char *encoder;   /* "h264_nvenc" / "libx264"; NULL -> nvenc then x264 */
    int cpu_only;          /* --cpu: force CPU scale/convert (no CUDA filter graph) */
    int use_vaapi;         /* --vaapi: encode on the iGPU via VAAPI (frees the dGPU; AMD = radeonsi) */
    int use_kmsgrab;       /* --kmsgrab: capture via DRM/KMS instead of x11grab (zero-copy w/ --vaapi) */
    int enc_level;         /* encoder effort: 0 = quality (nvenc p7 + 2-pass / x264 veryfast),
                            * 1 = balanced (nvenc p6 + 1-pass / x264 faster),
                            * 2 = performance (nvenc p4 + 1-pass / x264 superfast). Lower GPU/CPU as it
                            * rises, at some quality cost. (was the old enc_perf bool.) */
    int enc_x264_threads;  /* opt-in (P6.9): >1 = N x264 FRAME threads on the live --cpu path (kept to
                            * one NAL/frame via slices=1); adds ~(N-1) frames latency. 0/1 = single. */
    /* Wayland: desktop capture backend selection (Linux). Default autodetect = try X11 (x11grab)
     * first, fall back to the xdg-desktop-portal + PipeWire path on a Wayland session. */
    int force_x11;         /* --x11: never use the Wayland portal (x11grab/kmsgrab only) */
    int force_pipewire;    /* --wayland / --pipewire: always use the portal + PipeWire path */
    int pw_dmabuf;         /* --pw-dmabuf (EXPERIMENTAL): negotiate dmabuf from PipeWire and import it
                            * zero-copy into VAAPI (needs --vaapi). Falls back to the CPU MAP_BUFFERS
                            * path if dmabuf negotiation or VAAPI import fails. Off by default. */
    const char *input_file;/* non-NULL: decode this file instead of grabbing the screen (re-encodes
                            * so an overlay can be composited). Forces the CPU scale path. */
    int loop;              /* file mode: seek back to 0 at EOF */
    /* Webcam source: when non-NULL (and no input_file), capture a camera instead of the screen via
     * the platform live-video input (Linux v4l2 "/dev/videoN", Windows dshow "video=Name", macOS
     * avfoundation index/name). Forces the CPU scale path (same as file mode). */
    const char *webcam;
    /* Stereo 3D: a SECOND camera. When both webcam+webcam_right are set the two live feeds are
     * composited side-by-side (real stereo, left=webcam right=webcam_right) — this bypasses the
     * depth-based 2D->3D synth. threed_swap swaps the eyes; threed_full = full-res per eye. */
    const char *webcam_right;
    /* 2D->3D side-by-side. When threed_mode != 0 the capture scales to a source NV12 buffer, then
     * synthesises the SBS frame the encoder gets. threed_full: 1 = full resolution per eye (the
     * encoded frame is twice as wide, each half a full-width eye); 0 = half-width squished eyes. */
    int threed_mode;        /* 0 off / 1 fast / 2 ai */
    int threed_deepness;    /* 0..100 */
    int threed_convergence; /* -50..50 */
    int threed_swap;        /* swap L/R */
    int threed_full;        /* 1 = full-res per eye (2x width) */
    int threed_tier;        /* AI in-process depth tier: 0 external/none, 1 cpu, 2 gpu, 3 hi */
    char threed_ai_cmd[256];/* AI-mode external depth helper command */
    /* Raw in-process frame source (terminal PTY backend): when raw_render is non-NULL the capture
     * pulls frames from it instead of grabbing a screen/decoding a file — like the PipeWire path but
     * CPU-only. raw_render fills a packed 32-bit BGR0 buffer (raw_w x raw_h, stride = raw_w*4) and
     * returns 0 on a frame, <0 at EOF. Forces the CPU scale path (composites the exit bar). */
    int (*raw_render)(void *user, uint8_t *bgr0, int w, int h);
    void *raw_user;
    int raw_w, raw_h;
} bsdr_capture_config;

bsdr_capture *bsdr_capture_open(const bsdr_capture_config *cfg);

/* Composite this overlay onto each frame before encoding (NULL = none). */
struct bsdr_overlay;
void bsdr_capture_set_overlay(bsdr_capture *c, struct bsdr_overlay *ov);

/* Decode an image file (jpg/png/…) to a freshly malloc'd packed-RGB buffer (caller frees). Sets
 * *w and *h. Returns 0 on success. Used to load the face-swap source image (via a host service, so the
 * face-swap plugin can reuse the core's FFmpeg image decoder). */
int bsdr_capture_decode_image_rgb(const char *path, uint8_t **rgb, int *w, int *h);

/* Grab + encode one frame. On a produced access unit, sets *au (valid until the
 * next call), *len, and *rtp_ts (90 kHz). Returns 1 = frame, 0 = none yet,
 * <0 = error. Blocks at most one frame interval. */
int bsdr_capture_frame(bsdr_capture *c, const uint8_t **au, size_t *len,
                       uint32_t *rtp_ts);

/* The encoder's out dimensions / chosen encoder name (diagnostics). */
void bsdr_capture_info(bsdr_capture *c, int *w, int *h, const char **enc);

/* Force the next encoded frame to be a keyframe (IDR). Thread-safe. Used to serve an on-demand
 * keyframe to a newly-joined cloud consumer instead of making it wait for the next scheduled GOP. */
void bsdr_capture_force_keyframe(bsdr_capture *c);

/* ---- File-source playback controls (no-ops unless opened with cfg.input_file) ---- */
/* Seek to a fraction (0..1) of the file. Thread-safe wrt bsdr_capture_frame. */
void bsdr_capture_seek(bsdr_capture *c, double frac);
/* Freeze/resume: while paused the last frame is re-encoded so the stream (and overlay) stay live. */
void bsdr_capture_set_paused(bsdr_capture *c, int paused);
int  bsdr_capture_is_paused(bsdr_capture *c);
/* Current playback position as a fraction (0..1); *seekable = whether the file has a known duration. */
double bsdr_capture_position(bsdr_capture *c, int *seekable);

void bsdr_capture_close(bsdr_capture *c);

#endif /* BSDR_CAPTURE_H */
