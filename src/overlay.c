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
/* In-VR control bar: NV12 compositing + click hit-testing. */
#include "bsdr/overlay.h"
#include "bsdr/platform.h"
#include "font6x8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bar geometry in normalized coords (the bar occupies the bottom strip). */
#define BAR_Y0   0.86
/* button x-ranges within [0,1] */
#define PP_X0 0.02
#define PP_X1 0.07
#define MC_X0 0.08
#define MC_X1 0.13
#define SK_X0 0.15
#define SK_X1 0.62
#define VD_X0 0.64
#define VD_X1 0.69
#define VB_X0 0.70
#define VB_X1 0.80
#define VU_X0 0.81
#define VU_X1 0.86
#define LP_X0 0.87
#define LP_X1 0.92
#define EX_X0 0.93
#define EX_X1 0.99

/* Balloon geometry: an ellipse in normalized space so the drawn shape and the
 * hit-test always agree regardless of frame aspect. rx/ry chosen to read round
 * on 16:9. */
#define BALLOON_RX 0.025
#define BALLOON_RY 0.035

#define HIST_N     16          /* retained feedback lines */
#define HIST_LEN   192         /* per line */
#define TOAST_MS   6000         /* how long a new line shows before auto-hiding */
#define HIST_SHOW  6            /* lines visible when the log is open */

struct bsdr_overlay {
    bool visible;
    bool playing;
    bool loop;         /* continuous file/playlist loop (button highlight) */
    int volume;        /* 0..100 */
    double position;   /* 0..1 */
    bool seekable;
    /* voice-command balloon (independent of the bar) */
    bool balloon;
    bool listening;
    bool confirm;      /* show Send / Cancel after a capture stops */
    bool working;      /* show the stop balloon while a command runs */
    double bx, by;     /* normalized center */
    /* feedback bubble + history log */
    bsdr_mutex *lock;
    char hist[HIST_N][HIST_LEN];
    int  hist_count;               /* total pushed (head = newest at (count-1)%N) */
    uint64_t toast_until;          /* show the newest line until this time */
    bool toast_pending;            /* a new line was pushed; render arms toast_until */
    bool hist_open;                /* history panel toggled open */
};

bsdr_overlay *bsdr_overlay_new(void) {
    bsdr_overlay *o = calloc(1, sizeof(*o));
    if (o) { o->visible = true; o->playing = true; o->volume = 80;
             o->bx = 0.5; o->by = 0.12; o->lock = bsdr_mutex_new(); }
    return o;
}
void bsdr_overlay_free(bsdr_overlay *o) {
    if (!o) return;
    if (o->lock) bsdr_mutex_free(o->lock);
    free(o);
}
void bsdr_overlay_set_visible(bsdr_overlay *o, bool v) { o->visible = v; }
bool bsdr_overlay_visible(bsdr_overlay *o) { return o->visible; }
void bsdr_overlay_set_playing(bsdr_overlay *o, bool p) { o->playing = p; }
void bsdr_overlay_set_loop(bsdr_overlay *o, bool l) { o->loop = l; }
void bsdr_overlay_set_volume(bsdr_overlay *o, int v) { o->volume = v < 0 ? 0 : v > 100 ? 100 : v; }
void bsdr_overlay_set_position(bsdr_overlay *o, double f, bool s) {
    o->position = f < 0 ? 0 : f > 1 ? 1 : f; o->seekable = s;
}

/* ---- NV12 drawing helpers (Y = luma, UV interleaved at half resolution) ---*/
static void fill_y(uint8_t *y, int ys, int w, int h, int x0, int y0, int x1, int y1, uint8_t v) {
    /* clamp: icon geometry can exceed the frame by a few px */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int r = y0; r < y1; r++)
        for (int c = x0; c < x1; c++)
            y[r * ys + c] = v;
}
/* set chroma to neutral gray (128,128) over a region so icons read as white */
static void neutral_uv(uint8_t *uv, int us, int w, int h, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int r = y0 / 2; r < y1 / 2; r++)
        for (int c = x0 / 2; c < x1 / 2; c++) {
            uv[r * us + c * 2] = 128;
            uv[r * us + c * 2 + 1] = 128;
        }
}
static int px(double n, int dim) {
    int v = (int)(n * dim);
    if (v < 0) v = 0;
    if (v >= dim) v = dim - 1;
    return v;
}

void bsdr_overlay_render_nv12(bsdr_overlay *o, uint8_t *y, int ys,
                              uint8_t *uv, int us, int w, int h) {
    if (!o->visible) return;
    int by0 = px(BAR_Y0, h);
    /* translucent dark bar: halve luma, desaturate chroma */
    for (int r = by0; r < h; r++)
        for (int c = 0; c < w; c++) y[r * ys + c] = (uint8_t)(y[r * ys + c] / 2 + 12);
    neutral_uv(uv, us, w, h, 0, by0, w, h);

    int cy = (by0 + h) / 2;          /* vertical center of the bar */
    int bh = h - by0;
    int ih = bh / 3;                 /* icon half-extent */
    const uint8_t W = 235;           /* icon "white" */

    /* play/pause */
    int ax0 = px(PP_X0, w), ax1 = px(PP_X1, w);
    if (o->playing) {                /* pause: two bars */
        int bw = (ax1 - ax0) / 4;
        fill_y(y, ys, w, h, ax0 + bw, cy - ih, ax0 + 2 * bw, cy + ih, W);
        fill_y(y, ys, w, h, ax1 - 2 * bw, cy - ih, ax1 - bw, cy + ih, W);
    } else {                         /* play: triangle */
        for (int r = -ih; r <= ih; r++) {
            int len = (int)((ax1 - ax0) * (1.0 - (double)abs(r) / ih));
            fill_y(y, ys, w, h, ax0, cy + r, ax0 + len, cy + r + 1, W);
        }
    }
    /* mic icon: a capsule body + a base line */
    int mx0 = px(MC_X0, w), mx1 = px(MC_X1, w), mxc = (mx0 + mx1) / 2;
    fill_y(y, ys, w, h, mxc - (mx1 - mx0) / 6, cy - ih, mxc + (mx1 - mx0) / 6, cy + ih / 3, W);
    fill_y(y, ys, w, h, mxc - (mx1 - mx0) / 4, cy + ih / 2, mxc + (mx1 - mx0) / 4, cy + ih / 2 + 2, W);
    fill_y(y, ys, w, h, mxc - 1, cy + ih / 3, mxc + 2, cy + ih / 2, W);
    /* seek/position bar */
    if (o->seekable) {
        int sx0 = px(SK_X0, w), sx1 = px(SK_X1, w);
        fill_y(y, ys, w, h, sx0, cy - 1, sx1, cy + 2, 90);                  /* track */
        int fill = sx0 + (int)((sx1 - sx0) * o->position);
        fill_y(y, ys, w, h, sx0, cy - 2, fill, cy + 3, W);                  /* progress */
        fill_y(y, ys, w, h, fill - 2, cy - ih, fill + 2, cy + ih, W);       /* knob */
    }
    /* volume - / bar / + */
    int vd0 = px(VD_X0, w), vd1 = px(VD_X1, w);
    fill_y(y, ys, w, h, vd0, cy - 1, vd1, cy + 2, W);                       /* minus */
    int vb0 = px(VB_X0, w), vb1 = px(VB_X1, w);
    fill_y(y, ys, w, h, vb0, cy - 1, vb1, cy + 2, 90);
    fill_y(y, ys, w, h, vb0, cy - 2, vb0 + (vb1 - vb0) * o->volume / 100, cy + 3, W);
    int vu0 = px(VU_X0, w), vu1 = px(VU_X1, w), vuc = (vu0 + vu1) / 2;
    fill_y(y, ys, w, h, vu0, cy - 1, vu1, cy + 2, W);                       /* plus - */
    fill_y(y, ys, w, h, vuc - 1, cy - ih, vuc + 2, cy + ih, W);            /* plus | */
    /* loop toggle: a rectangle outline (repeat), bright when on, dim when off */
    int lp0 = px(LP_X0, w), lp1 = px(LP_X1, w);
    uint8_t lw = o->loop ? W : 100;
    fill_y(y, ys, w, h, lp0, cy - ih, lp1, cy - ih + 2, lw);            /* top */
    fill_y(y, ys, w, h, lp0, cy + ih - 2, lp1, cy + ih, lw);            /* bottom */
    fill_y(y, ys, w, h, lp0, cy - ih, lp0 + 2, cy + ih, lw);            /* left */
    fill_y(y, ys, w, h, lp1 - 2, cy - ih, lp1, cy + ih, lw);            /* right */
    /* exit: X */
    int ex0 = px(EX_X0, w), ex1 = px(EX_X1, w);
    for (int r = -ih; r <= ih; r++) {
        int t = (ex1 - ex0) * (r + ih) / (2 * ih);
        fill_y(y, ys, w, h, ex0 + t, cy + r, ex0 + t + 2, cy + r + 1, W);
        fill_y(y, ys, w, h, ex1 - t - 2, cy + r, ex1 - t, cy + r + 1, W);
    }
}

bsdr_overlay_action bsdr_overlay_hit(bsdr_overlay *o, double nx, double ny,
                                     double *value) {
    if (value) *value = 0;
    if (!o->visible || ny < BAR_Y0) return BSDR_OVL_NONE;
    if (nx >= PP_X0 && nx <= PP_X1) return BSDR_OVL_PLAYPAUSE;
    if (nx >= MC_X0 && nx <= MC_X1) return BSDR_OVL_VOICE;
    if (o->seekable && nx >= SK_X0 && nx <= SK_X1) {
        if (value) *value = (nx - SK_X0) / (SK_X1 - SK_X0);
        return BSDR_OVL_SEEK;
    }
    if (nx >= LP_X0 && nx <= LP_X1) return BSDR_OVL_LOOP;
    if (nx >= VD_X0 && nx <= VD_X1) return BSDR_OVL_VOL_DOWN;
    if (nx >= VU_X0 && nx <= VU_X1) return BSDR_OVL_VOL_UP;
    if (nx >= VB_X0 && nx <= VB_X1) {   /* click on the volume bar = set level */
        if (value) *value = (nx - VB_X0) / (VB_X1 - VB_X0);
        return BSDR_OVL_VOL_UP;          /* caller maps value to volume */
    }
    if (nx >= EX_X0 && nx <= EX_X1) return BSDR_OVL_EXIT;
    return BSDR_OVL_NONE;
}

/* ---- Voice-command balloon ------------------------------------------------ */
/* Button/handle geometry, normalized, relative to the balloon center. Kept in one
 * place so drawing and hit-testing agree. */
#define HANDLE_DY   (BALLOON_RY + 0.0225)  /* log handle, below the balloon */
#define HANDLE_HX   0.0175
#define HANDLE_HY   0.011
#define BTN_DY      (BALLOON_RY + 0.0375)  /* Send / Cancel row */
#define BTN_OFF     0.0475                 /* x offset of each button center */
#define BTN_HX      0.04
#define BTN_HY      0.016
#define STOP_DX     0.06                   /* stop balloon offset from the main balloon */
#define STOP_RX     0.02
#define STOP_RY     0.0275

/* Stop-balloon center x: to whichever side has room, so it stays on-screen. */
static double stop_nx(const bsdr_overlay *o) {
    return o->bx < 0.5 ? o->bx + STOP_DX : o->bx - STOP_DX;
}

void bsdr_overlay_set_balloon(bsdr_overlay *o, bool on) { o->balloon = on; }
bool bsdr_overlay_balloon_on(bsdr_overlay *o) { return o->balloon; }
void bsdr_overlay_set_listening(bsdr_overlay *o, bool l) { o->listening = l; }
void bsdr_overlay_set_confirm(bsdr_overlay *o, bool c) { o->confirm = c; }
void bsdr_overlay_set_working(bsdr_overlay *o, bool wk) { o->working = wk; }
bool bsdr_overlay_stop_hit(bsdr_overlay *o, double nx, double ny) {
    if (!o->balloon || !o->working) return false;
    double dx = (nx - stop_nx(o)) / STOP_RX, dy = (ny - o->by) / STOP_RY;
    return dx * dx + dy * dy <= 1.0;
}
void bsdr_overlay_set_balloon_pos(bsdr_overlay *o, double nx, double ny) {
    if (nx < BALLOON_RX) nx = BALLOON_RX;
    if (nx > 1 - BALLOON_RX) nx = 1 - BALLOON_RX;
    if (ny < BALLOON_RY) ny = BALLOON_RY;
    if (ny > 1 - BALLOON_RY) ny = 1 - BALLOON_RY;
    o->bx = nx; o->by = ny;
}
void bsdr_overlay_get_balloon_pos(bsdr_overlay *o, double *nx, double *ny) {
    if (nx) *nx = o->bx;
    if (ny) *ny = o->by;
}
bool bsdr_overlay_balloon_hit(bsdr_overlay *o, double nx, double ny) {
    if (!o->balloon) return false;
    double dx = (nx - o->bx) / BALLOON_RX, dy = (ny - o->by) / BALLOON_RY;
    return dx * dx + dy * dy <= 1.0;
}
bool bsdr_overlay_history_hit(bsdr_overlay *o, double nx, double ny) {
    if (!o->balloon) return false;
    return nx > o->bx - HANDLE_HX && nx < o->bx + HANDLE_HX &&
           ny > o->by + HANDLE_DY - HANDLE_HY && ny < o->by + HANDLE_DY + HANDLE_HY;
}
/* 0 = none, 1 = Send, 2 = Cancel (only while the confirm row is shown). */
int bsdr_overlay_confirm_hit(bsdr_overlay *o, double nx, double ny) {
    if (!o->balloon || !o->confirm) return 0;
    if (ny < o->by + BTN_DY - BTN_HY || ny > o->by + BTN_DY + BTN_HY) return 0;
    if (nx > o->bx - BTN_OFF - BTN_HX && nx < o->bx - BTN_OFF + BTN_HX) return 1;
    if (nx > o->bx + BTN_OFF - BTN_HX && nx < o->bx + BTN_OFF + BTN_HX) return 2;
    return 0;
}

void bsdr_overlay_toggle_history(bsdr_overlay *o) {
    bsdr_mutex_lock(o->lock);
    o->hist_open = !o->hist_open;
    bsdr_mutex_unlock(o->lock);
}
void bsdr_overlay_push_feedback(bsdr_overlay *o, const char *text) {
    if (!text) return;
    bsdr_mutex_lock(o->lock);
    snprintf(o->hist[o->hist_count % HIST_N], HIST_LEN, "%s", text);
    o->hist_count++;
    o->toast_pending = true;
    bsdr_mutex_unlock(o->lock);
}

/* Alpha-blend one 32-bit packed pixel (b/g/r channels) toward (br,bg,bb). */
static void blend_px(uint8_t *p, int bgr, int br, int bg, int bb, int a) {
    int ib = bgr ? 0 : 2, ir = bgr ? 2 : 0;
    p[ib] = (uint8_t)((bb * a + p[ib] * (255 - a)) / 255);
    p[1]  = (uint8_t)((bg * a + p[1]  * (255 - a)) / 255);
    p[ir] = (uint8_t)((br * a + p[ir] * (255 - a)) / 255);
}
static void fill_rect(uint8_t *d, int stride, int w, int h, int bgr,
                      int x0, int y0, int x1, int y1, int R, int G, int B, int a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            blend_px(d + (size_t)y * stride + (size_t)x * 4, bgr, R, G, B, a);
}
/* Draw one glyph (scaled). */
static void draw_char(uint8_t *d, int stride, int w, int h, int bgr,
                      int x0, int y0, int scale, char c, int R, int G, int B) {
    if (c < 0x20 || c > 0x7e) c = '?';
    const uint8_t *g = FONT6X8[c - 0x20];
    for (int ry = 0; ry < BSDR_FONT_H; ry++)
        for (int rx = 0; rx < BSDR_FONT_W; rx++) {
            if (!((g[ry] >> rx) & 1)) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int x = x0 + rx * scale + sx, y = y0 + ry * scale + sy;
                    if (x >= 0 && x < w && y >= 0 && y < h)
                        blend_px(d + (size_t)y * stride + (size_t)x * 4, bgr, R, G, B, 255);
                }
        }
}
/* Draw a string; returns the x past the last glyph. `max_chars` truncates. */
static int draw_text(uint8_t *d, int stride, int w, int h, int bgr,
                     int x0, int y0, int scale, const char *s, int max_chars,
                     int R, int G, int B) {
    int x = x0, n = 0, adv = (BSDR_FONT_W + 1) * scale;
    for (; *s && n < max_chars; s++, n++) { draw_char(d, stride, w, h, bgr, x, y0, scale, *s, R, G, B); x += adv; }
    return x;
}

/* A rounded-ish label button. */
static void draw_button(uint8_t *d, int stride, int w, int h, int bgr,
                        int cx, int cy, int hw, int hh, int scale, const char *label,
                        int R, int G, int B) {
    fill_rect(d, stride, w, h, bgr, cx - hw, cy - hh, cx + hw, cy + hh, R, G, B, 210);
    fill_rect(d, stride, w, h, bgr, cx - hw, cy - hh, cx + hw, cy - hh + 1, 255,255,255, 120);
    int tw = (int)strlen(label) * (BSDR_FONT_W + 1) * scale;
    draw_text(d, stride, w, h, bgr, cx - tw / 2, cy - (BSDR_FONT_H * scale) / 2, scale, label, 32, 255,255,255);
}

void bsdr_overlay_render_balloon(bsdr_overlay *o, uint8_t *data, int stride,
                                 int w, int h, int bgr_order, uint64_t now_ms) {
    if (!o->balloon || w <= 0 || h <= 0) return;
    int cx = px(o->bx, w), cy = px(o->by, h);
    int rx = (int)(BALLOON_RX * w), ry = (int)(BALLOON_RY * h);
    if (rx < 2 || ry < 2) return;
    int scale = w / 700; if (scale < 1) scale = 1; if (scale > 4) scale = 4;
    int ch = (BSDR_FONT_H + 2) * scale;                 /* text line height */

    /* body color: cyan idle, red listening, amber while confirming */
    int R = o->listening ? 235 : (o->confirm ? 240 : 40);
    int G = o->listening ? 40  : (o->confirm ? 170 : 200);
    int B = o->listening ? 40  : (o->confirm ? 40  : 235);
    for (int dy = -ry; dy <= ry; dy++) {
        int y = cy + dy; if (y < 0 || y >= h) continue;
        double ny = (double)dy / ry;
        for (int dx = -rx; dx <= rx; dx++) {
            int x = cx + dx; if (x < 0 || x >= w) continue;
            double nx = (double)dx / rx;
            double dd = nx * nx + ny * ny;
            if (dd > 1.0) continue;
            uint8_t *p = data + (size_t)y * stride + (size_t)x * 4;
            if (dd > 0.82) { blend_px(p, bgr_order, 250, 250, 250, 235); continue; } /* white rim */
            blend_px(p, bgr_order, R, G, B, 190);                                    /* body */
        }
    }
    /* mic glyph (white): capsule body + stand + base */
    int gw = rx / 3, gh = ry / 2;
    for (int y = cy - gh; y <= cy + gh / 3; y++)
        for (int x = cx - gw; x <= cx + gw; x++)
            if (x >= 0 && x < w && y >= 0 && y < h)
                blend_px(data + (size_t)y * stride + (size_t)x * 4, bgr_order, 255,255,255, 255);
    for (int y = cy + gh / 3; y <= cy + gh; y++)
        if (y >= 0 && y < h && cx >= 0 && cx < w)
            blend_px(data + (size_t)y * stride + (size_t)cx * 4, bgr_order, 255,255,255, 255);
    for (int x = cx - gw; x <= cx + gw; x++)
        if (x >= 0 && x < w && cy + gh >= 0 && cy + gh < h)
            blend_px(data + (size_t)(cy + gh) * stride + (size_t)x * 4, bgr_order, 255,255,255, 255);

    /* log handle (three lines) under the balloon */
    int hy = cy + (int)(HANDLE_DY * h);
    int hhx = (int)(HANDLE_HX * w), hhy = (int)(HANDLE_HY * h);
    fill_rect(data, stride, w, h, bgr_order, cx - hhx, hy - hhy, cx + hhx, hy + hhy, 20, 24, 32, 150);
    for (int i = -1; i <= 1; i++)
        fill_rect(data, stride, w, h, bgr_order, cx - hhx/2, hy + i*scale*2 - scale/2,
                  cx + hhx/2, hy + i*scale*2 + (scale+1)/2, 210,210,220, 220);

    /* Stop balloon (red, white square) while a command runs */
    if (o->working) {
        int scx = px(stop_nx(o), w), scy = cy;
        int srx = (int)(STOP_RX * w), sry = (int)(STOP_RY * h);
        for (int dy = -sry; dy <= sry; dy++) {
            int y = scy + dy; if (y < 0 || y >= h) continue;
            double ny = (double)dy / sry;
            for (int dx = -srx; dx <= srx; dx++) {
                int x = scx + dx; if (x < 0 || x >= w) continue;
                double nx = (double)dx / srx, dd = nx * nx + ny * ny;
                if (dd > 1.0) continue;
                uint8_t *p = data + (size_t)y * stride + (size_t)x * 4;
                if (dd > 0.80) blend_px(p, bgr_order, 250,250,250, 235);
                else blend_px(p, bgr_order, 220, 45, 45, 200);
            }
        }
        int sq = srx / 2;                              /* white stop square */
        fill_rect(data, stride, w, h, bgr_order, scx - sq, scy - sq, scx + sq, scy + sq, 255,255,255, 255);
    }

    /* Confirm row: Send / Cancel */
    if (o->confirm) {
        int by = cy + (int)(BTN_DY * h), bhx = (int)(BTN_HX * w), bhy = (int)(BTN_HY * h);
        int off = (int)(BTN_OFF * w);
        draw_button(data, stride, w, h, bgr_order, cx - off, by, bhx, bhy, scale, "SEND",   36,150,60);
        draw_button(data, stride, w, h, bgr_order, cx + off, by, bhx, bhy, scale, "CANCEL", 160,50,60);
    }

    /* Feedback bubble / history panel. Arm the toast on the first render after a push. */
    bsdr_mutex_lock(o->lock);
    if (o->toast_pending) { o->toast_until = now_ms + TOAST_MS; o->toast_pending = false; }
    bool open = o->hist_open;
    bool toast = now_ms < o->toast_until;
    if (o->hist_count && (open || toast)) {
        int nlines = open ? (o->hist_count < HIST_SHOW ? o->hist_count : HIST_SHOW) : 1;
        int maxchars = (w * 6 / 10) / ((BSDR_FONT_W + 1) * scale);   /* fit ~60% width */
        if (maxchars < 8) maxchars = 8;
        if (maxchars > HIST_LEN - 1) maxchars = HIST_LEN - 1;
        int pw = maxchars * (BSDR_FONT_W + 1) * scale + 12 * scale;
        int ph = nlines * ch + (open ? ch : 0) + 6 * scale;
        int px0 = cx - pw / 2;
        int py0 = cy + (int)((o->confirm ? BTN_DY + 0.05 : HANDLE_DY + 0.03) * h);
        if (px0 < 4) px0 = 4;
        if (px0 + pw > w - 4) px0 = w - 4 - pw;
        fill_rect(data, stride, w, h, bgr_order, px0, py0, px0 + pw, py0 + ph, 12, 15, 22, 205);
        fill_rect(data, stride, w, h, bgr_order, px0, py0, px0 + pw, py0 + 1, 90,120,200, 200);
        int ty = py0 + 4 * scale, tx = px0 + 6 * scale;
        if (open) {
            draw_text(data, stride, w, h, bgr_order, tx, ty, scale, "HISTORY (tap log to close)", maxchars, 150,170,210);
            ty += ch;
            int start = o->hist_count > HIST_SHOW ? o->hist_count - HIST_SHOW : 0;
            for (int i = start; i < o->hist_count; i++) {
                draw_text(data, stride, w, h, bgr_order, tx, ty, scale, o->hist[i % HIST_N], maxchars, 225,228,235);
                ty += ch;
            }
        } else {
            const char *latest = o->hist[(o->hist_count - 1) % HIST_N];
            draw_text(data, stride, w, h, bgr_order, tx, ty, scale, latest, maxchars, 225,228,235);
        }
    }
    bsdr_mutex_unlock(o->lock);
}
