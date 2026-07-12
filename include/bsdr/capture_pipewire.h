/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Wayland desktop capture via xdg-desktop-portal ScreenCast + PipeWire.
 *
 * X11's x11grab can't see a native Wayland desktop (it only grabs the XWayland root, which is
 * usually blank). The portal is the sanctioned Wayland path: an org.freedesktop.portal.ScreenCast
 * session negotiates a PipeWire node (the compositor's screencast), which we consume as raw video
 * frames and feed into capture.c's existing scale/encode pipeline exactly where x11grab's decoded
 * frames land (packed 32-bit RGB, so no pipeline change). Built only when libpipewire-0.3 + dbus-1
 * are present (BSDR_HAVE_PIPEWIRE); a stub otherwise. */
#ifndef BSDR_CAPTURE_PIPEWIRE_H
#define BSDR_CAPTURE_PIPEWIRE_H

#include <stdint.h>

typedef struct bsdr_pw_capture bsdr_pw_capture;

/* Frame pixel layout the module delivers (matches an ffmpeg AVPixelFormat family). */
typedef enum {
    BSDR_PW_FMT_BGR0 = 0,   /* == AV_PIX_FMT_BGR0 (x11grab's format; the common case) */
    BSDR_PW_FMT_RGB0,       /* == AV_PIX_FMT_RGB0 */
    BSDR_PW_FMT_BGRA,       /* == AV_PIX_FMT_BGRA */
    BSDR_PW_FMT_RGBA        /* == AV_PIX_FMT_RGBA */
} bsdr_pw_format;

/* 1 if this build has the portal/PipeWire backend compiled in. */
int bsdr_pw_capture_available(void);

/* Open a screencast. want_window: 0 = a whole monitor, 1 = ask the portal to let the user pick a
 * window. cursor: 0 = hidden, 1 = embedded (composited into the frames). Blocks on the portal
 * dialog. On success returns a handle and fills w, h (negotiated size) + fmt; NULL on failure
 * (no portal, user cancelled, no PipeWire, or not compiled in). */
bsdr_pw_capture *bsdr_pw_capture_open(int want_window, int cursor, int *w, int *h, bsdr_pw_format *fmt);

/* Same, but also request dmabuf negotiation (EXPERIMENTAL, --pw-dmabuf) for zero-copy import into
 * VAAPI. Falls back to the CPU MAP_BUFFERS path if the compositor/driver can't provide dmabuf. Use
 * bsdr_pw_capture_dmabuf_active() AFTER open to learn which path was actually negotiated. */
bsdr_pw_capture *bsdr_pw_capture_open2(int want_window, int cursor, int want_dmabuf,
                                       int *w, int *h, bsdr_pw_format *fmt);

/* 1 if a dmabuf (zero-copy) stream was negotiated. When 0, use bsdr_pw_capture_read (CPU frames). */
int bsdr_pw_capture_dmabuf_active(bsdr_pw_capture *c);

/* The hw_frames_ctx (AVBufferRef*, DRM_PRIME) to install into the decoder ctx so ffmpeg's hwmap can
 * import the borrowed dmabufs. Returns NULL unless dmabuf is active. Cast to AVBufferRef*. */
void *bsdr_pw_capture_drm_frames_ctx(bsdr_pw_capture *c);

/* Borrow the newest dmabuf frame as a DRM_PRIME AVFrame (cast the return to AVFrame*). Its buf[0]
 * free callback releases the underlying PipeWire buffer, so feed it to a filtergraph / unref it
 * promptly. Returns NULL if no new frame or dmabuf isn't active. Only valid when _dmabuf_active. */
void *bsdr_pw_capture_read_drm(bsdr_pw_capture *c);

/* Borrow the most recent frame (packed 32-bit RGB, the format from _open) — zero copy. On success
 * *out_frame points at an internal buffer the caller may read (and draw on in place) until the NEXT
 * bsdr_pw_capture_read call; *out_stride/out_w/out_h describe it. A triple-buffer handoff guarantees
 * the capture thread never writes the borrowed slot in that window. Waits up to ~100 ms for a fresh
 * frame. Returns 1 if a frame is ready, 0 if none arrived (retry), -1 on a closed/fatal stream. */
int bsdr_pw_capture_read(bsdr_pw_capture *c, const uint8_t **out_frame,
                         int *out_stride, int *out_w, int *out_h);

void bsdr_pw_capture_close(bsdr_pw_capture *c);

#endif /* BSDR_CAPTURE_PIPEWIRE_H */
