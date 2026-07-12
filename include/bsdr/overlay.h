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
/* In-VR control bar composited into the outgoing video; clicks arrive over the
 * input channel (absolute mouse position) and are hit-tested here. Buttons:
 * play/pause, seek/position (file), volume down/up, exit (back to desktop). */
#ifndef BSDR_OVERLAY_H
#define BSDR_OVERLAY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BSDR_OVL_NONE = 0,
    BSDR_OVL_PLAYPAUSE,
    BSDR_OVL_VOL_DOWN,
    BSDR_OVL_VOL_UP,
    BSDR_OVL_SEEK,        /* value = 0..1 position */
    BSDR_OVL_LOOP,        /* toggle continuous file/playlist loop */
    BSDR_OVL_EXIT,
    BSDR_OVL_VOICE        /* mic: start a voice command */
} bsdr_overlay_action;

typedef struct bsdr_overlay bsdr_overlay;

bsdr_overlay *bsdr_overlay_new(void);
void bsdr_overlay_free(bsdr_overlay *o);

void bsdr_overlay_set_visible(bsdr_overlay *o, bool visible);
bool bsdr_overlay_visible(bsdr_overlay *o);
void bsdr_overlay_set_playing(bsdr_overlay *o, bool playing);
void bsdr_overlay_set_loop(bsdr_overlay *o, bool loop);   /* highlight the loop button when on */
void bsdr_overlay_set_volume(bsdr_overlay *o, int vol_0_100);
/* `seekable` shows the position bar (file playback); frac is 0..1. */
void bsdr_overlay_set_position(bsdr_overlay *o, double frac, bool seekable);

/* Composite the bar onto an NV12 frame (Y plane + interleaved UV plane). */
void bsdr_overlay_render_nv12(bsdr_overlay *o, uint8_t *y, int ystride,
                              uint8_t *uv, int uvstride, int w, int h);

/* Hit-test a normalized click (0..1). Returns the action; *value gets the seek
 * fraction for BSDR_OVL_SEEK. Returns NONE if not on the bar. */
bsdr_overlay_action bsdr_overlay_hit(bsdr_overlay *o, double nx, double ny,
                                     double *value);

/* ---- Voice-command balloon --------------------------------------------------
 * A small draggable icon composited over the desktop the operator streams to the
 * headset. The Quest pointer can drag it anywhere and a click on it starts a
 * listen-until-silence voice command. Independent of the control bar above; the
 * balloon is drawn onto the *source* frame (packed 32-bit BGR/RGB) so it survives
 * both the CPU and the GPU (VAAPI/CUDA) encode paths. */
void bsdr_overlay_set_balloon(bsdr_overlay *o, bool on);
bool bsdr_overlay_balloon_on(bsdr_overlay *o);
/* Balloon center in normalized [0,1] coords (clamped to keep it on-screen). */
void bsdr_overlay_set_balloon_pos(bsdr_overlay *o, double nx, double ny);
void bsdr_overlay_get_balloon_pos(bsdr_overlay *o, double *nx, double *ny);
/* Red "listening" glow while a voice capture is in progress. */
void bsdr_overlay_set_listening(bsdr_overlay *o, bool listening);
/* Show the Send / Cancel row after a capture stops (amber balloon). */
void bsdr_overlay_set_confirm(bsdr_overlay *o, bool confirm);
/* Show the stop balloon while a command runs (click it to abort). */
void bsdr_overlay_set_working(bsdr_overlay *o, bool working);
/* True if a normalized point lands on the balloon (same ellipse that's drawn). */
bool bsdr_overlay_balloon_hit(bsdr_overlay *o, double nx, double ny);
/* True if a normalized point lands on the stop balloon (only while shown). */
bool bsdr_overlay_stop_hit(bsdr_overlay *o, double nx, double ny);
/* 0 = none, 1 = Send, 2 = Cancel — hit-test the confirm row (only while shown). */
int bsdr_overlay_confirm_hit(bsdr_overlay *o, double nx, double ny);

/* ---- Feedback bubble + history log (under the balloon) ----------------------
 * Push a status/thinking/result line: it shows for a few seconds under the balloon
 * then auto-hides, and is retained in a scrollback the user can re-open by clicking
 * the small log handle beneath the balloon. Thread-safe. */
void bsdr_overlay_push_feedback(bsdr_overlay *o, const char *text);
/* Toggle the history panel open/closed (the log handle). */
void bsdr_overlay_toggle_history(bsdr_overlay *o);
/* True if a normalized point lands on the log handle under the balloon. */
bool bsdr_overlay_history_hit(bsdr_overlay *o, double nx, double ny);

/* Composite the balloon (+ feedback bubble / history) onto a packed 32-bit source
 * frame (4 bytes/pixel). `bgr_order` = 1 for BGR0/BGRA (x11grab), 0 for RGB0/RGBA.
 * `now_ms` is a monotonic clock used to expire the transient bubble. No-op unless
 * the balloon is on. */
void bsdr_overlay_render_balloon(bsdr_overlay *o, uint8_t *data, int stride,
                                 int w, int h, int bgr_order, uint64_t now_ms);

#endif /* BSDR_OVERLAY_H */
