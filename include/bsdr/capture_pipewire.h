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

/* Copy the most recent frame into dst (packed 32-bit RGB, the format from _open), row by row into a
 * dst_stride-byte pitch for dst_rows rows (padding/cropping the stride difference safely). Waits up
 * to ~100 ms for a fresh frame. Returns 1 if a frame was copied, 0 if none arrived (retry), -1 on a
 * closed/fatal stream. */
int bsdr_pw_capture_read(bsdr_pw_capture *c, uint8_t *dst, int dst_stride, int dst_rows);

void bsdr_pw_capture_close(bsdr_pw_capture *c);

#endif /* BSDR_CAPTURE_PIPEWIRE_H */
