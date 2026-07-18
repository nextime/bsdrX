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
/* Terminal source: stream a shell/console to the headset and inject the Quest's keyboard+mouse into
 * it. Two backends, both usable on a HEADLESS machine (no monitor):
 *   - PTY  : an in-process terminal emulator (libvterm) rendered straight to video frames. Needs no
 *            X server at all. Keystrokes go to the pty; mouse is forwarded only when the running
 *            program turns on terminal mouse reporting ("when supported").
 *   - XVFB : a private Xvfb + xterm captured with x11grab; input injected into that display via
 *            XTEST. A full graphical terminal with real mouse support. Needs Xvfb + xterm installed.
 * The agent owns one bsdr_term for the active session and routes decoded input events to it. */
#ifndef BSDR_TERM_H
#define BSDR_TERM_H

#include <stdint.h>
#include "bsdr/events.h"

typedef struct bsdr_term bsdr_term;

typedef enum { BSDR_TERM_PTY = 0, BSDR_TERM_XVFB = 1 } bsdr_term_backend;

typedef struct {
    bsdr_term_backend backend;
    const char *cmd;      /* shell/program to run (NULL -> $SHELL, else /bin/bash) */
    int cols, rows;       /* PTY grid (<=0 -> 120x36) */
    int width, height;    /* desired pixel size (XVFB screen; PTY render target hint); <=0 -> 1280x720 */
    int desktop;          /* XVFB only: run a full virtual desktop (window manager + terminal) instead of
                           * a bare fullscreen xterm. With `cmd` set, `cmd` is the whole session command
                           * (e.g. "startxfce4"); else a WM is auto-detected (openbox/fluxbox/icewm/...). */
} bsdr_term_config;

/* Start the backend (spawns the child process). Returns NULL on failure. */
bsdr_term *bsdr_term_start(const bsdr_term_config *cfg);

/* XVFB: the X display string (e.g. ":99") to hand to x11grab. NULL for the PTY backend. */
const char *bsdr_term_display(bsdr_term *t);

/* Non-zero for the in-process PTY backend (frames come from bsdr_term_render, not a display). */
int bsdr_term_is_pty(bsdr_term *t);

/* PTY: natural render size in pixels (cols*cellw x rows*cellh). */
void bsdr_term_size(bsdr_term *t, int *w, int *h);

/* PTY frame producer (matches bsdr_capture_config.raw_render): fill a packed 32-bit XRGB buffer
 * (BUS little-endian B,G,R,X per pixel — i.e. AV_PIX_FMT_BGR0) of w*h, stride = w*4. Returns 0 on a
 * produced frame, <0 when the shell has exited (EOF -> caller falls back to desktop). */
int bsdr_term_render(void *term, uint8_t *bgr0, int w, int h);

/* Inject one decoded input event. Handles both backends internally (XVFB -> XTEST to its display;
 * PTY -> key/mouse byte sequences to the master, mouse only when the app enabled reporting). */
void bsdr_term_input(bsdr_term *t, const bsdr_input_event *ev);

/* Non-zero once the child shell/terminal has exited (agent then returns to the desktop source). */
int bsdr_term_dead(bsdr_term *t);

void bsdr_term_stop(bsdr_term *t);

#endif /* BSDR_TERM_H */
