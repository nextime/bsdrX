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
/* PTY terminal backend: an in-process terminal emulator (vendored libvterm) driving a shell over a
 * pseudo-terminal, rendered straight to video frames with the built-in 6x8 font. Needs NO X server —
 * this is the truly-headless path. Keystrokes go to the pty; mouse is forwarded only when the running
 * program turns on terminal mouse reporting (libvterm gates that for us). See src/term.c for the
 * dispatcher and include/bsdr/term.h for the public API. */
#if defined(__linux__) && !defined(BSDR_PLATFORM_ANDROID) && defined(BSDR_HAVE_VTERM)

#define _GNU_SOURCE
#include "bsdr/events.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"

#include "vterm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pty.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#include "font6x8.h"

struct bsdr_term_pty {
    int master;                  /* pty master fd */
    pid_t child;
    VTerm *vt;
    VTermScreen *vts;
    VTermState *vstate;
    int cols, rows;
    int scale;                   /* integer font scale; cell = 6*scale x 8*scale px */
    int cellw, cellh, px_w, px_h;
    bsdr_mutex *lock;            /* guards all vt access (reader thread writes, render/input read) */
    bsdr_thread *reader;
    volatile int dead;
    volatile int stop;
    /* modifier latch: the Quest sends Ctrl/Shift/Alt as momentary taps and expects them held across
     * the next key (same as the uinput/XTEST paths). */
    int mod;
    unsigned long long latch_ms;
};

static unsigned long long mono_ms(void) {
    return bsdr_now_ms();
}

/* libvterm output (query replies + the key/mouse byte sequences it generates) -> pty master. */
static void out_cb(const char *s, size_t len, void *user) {
    struct bsdr_term_pty *p = (struct bsdr_term_pty *)user;
    ssize_t off = 0;
    while (len > 0) {
        ssize_t w = write(p->master, s + off, len);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        off += w; len -= (size_t)w;
    }
}

/* Reader thread: pty master -> vterm. */
static void reader_main(void *arg) {
    struct bsdr_term_pty *p = (struct bsdr_term_pty *)arg;
    char buf[4096];
    while (!p->stop) {
        ssize_t n = read(p->master, buf, sizeof buf);
        if (n > 0) {
            bsdr_mutex_lock(p->lock);
            vterm_input_write(p->vt, buf, (size_t)n);
            bsdr_mutex_unlock(p->lock);
        } else if (n == 0) { p->dead = 1; break; }
        else { if (errno == EINTR) continue; p->dead = 1; break; }
    }
}

struct bsdr_term_pty *bsdr_term_pty_start(const char *cmd, int cols, int rows) {
    if (cols <= 0) cols = 120;
    if (rows <= 0) rows = 36;
    struct bsdr_term_pty *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cols = cols; p->rows = rows;
    /* Pick an integer font scale so the rendered frame is a comfortable height (~720p). */
    p->scale = 720 / (rows * BSDR_FONT_H); if (p->scale < 1) p->scale = 1; if (p->scale > 6) p->scale = 6;
    p->cellw = BSDR_FONT_W * p->scale; p->cellh = BSDR_FONT_H * p->scale;
    p->px_w = cols * p->cellw; p->px_h = rows * p->cellh;

    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols,
                          .ws_xpixel = (unsigned short)p->px_w, .ws_ypixel = (unsigned short)p->px_h };
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, &ws);
    if (child < 0) { BSDR_ERROR("bsdr.term", "forkpty failed: %s", strerror(errno)); free(p); return NULL; }
    if (child == 0) {
        /* child: exec the shell */
        setenv("TERM", "xterm-256color", 1);
        char cs[16], rs[16]; snprintf(cs, sizeof cs, "%d", cols); snprintf(rs, sizeof rs, "%d", rows);
        setenv("COLUMNS", cs, 1); setenv("LINES", rs, 1);
        signal(SIGPIPE, SIG_DFL);
        const char *sh = (cmd && cmd[0]) ? NULL : (getenv("SHELL") ? getenv("SHELL") : "/bin/bash");
        if (sh) execlp(sh, sh, "-il", (char*)NULL);
        else    execlp("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    p->master = master;
    p->child = child;
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) & ~O_NONBLOCK);   /* blocking reads in the thread */

    p->vt = vterm_new(rows, cols);
    if (!p->vt) { close(master); kill(child, SIGKILL); free(p); return NULL; }
    vterm_set_utf8(p->vt, 1);
    vterm_output_set_callback(p->vt, out_cb, p);
    p->vts = vterm_obtain_screen(p->vt);
    p->vstate = vterm_obtain_state(p->vt);
    vterm_screen_reset(p->vts, 1);
    vterm_screen_enable_altscreen(p->vts, 1);

    p->lock = bsdr_mutex_new();
    p->reader = bsdr_thread_start(reader_main, p);
    BSDR_INFO("bsdr.term", "pty terminal up: %dx%d cells, %dx%d px (scale %d) -> in-process render",
              cols, rows, p->px_w, p->px_h, p->scale);
    return p;
}

void bsdr_term_pty_size(struct bsdr_term_pty *p, int *w, int *h) {
    if (w) *w = p ? p->px_w : 0;
    if (h) *h = p ? p->px_h : 0;
}

/* Resolve a VTermColor to 8-bit RGB. */
static void col_rgb(struct bsdr_term_pty *p, VTermColor *c, int deflt_fg, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (VTERM_COLOR_IS_DEFAULT_FG(c) && deflt_fg) { *r = *g = *b = 0xcc; return; }
    if (VTERM_COLOR_IS_DEFAULT_BG(c) && !deflt_fg) { *r = *g = *b = 0x00; return; }
    VTermColor cc = *c;
    vterm_screen_convert_color_to_rgb(p->vts, &cc);
    *r = cc.rgb.red; *g = cc.rgb.green; *b = cc.rgb.blue;
}

/* Render the current screen into a packed 32-bit BGR0 buffer (stride = w*4). Returns 0, or <0 if
 * the shell has exited. */
int bsdr_term_pty_render(struct bsdr_term_pty *p, uint8_t *bgr0, int w, int h) {
    if (!p) return -1;
    if (p->dead) return -1;
    bsdr_mutex_lock(p->lock);
    VTermPos cur; vterm_state_get_cursorpos(p->vstate, &cur);
    int S = p->scale;
    for (int cr = 0; cr < p->rows; cr++) {
        for (int cc = 0; cc < p->cols; cc++) {
            VTermPos pos = { .row = cr, .col = cc };
            VTermScreenCell cell;
            uint8_t fr=0xcc,fg=0xcc,fb=0xcc, br=0,bg=0,bb=0;
            uint32_t ch = ' ';
            int reverse = 0, is_cursor = (cr == cur.row && cc == cur.col);
            if (vterm_screen_get_cell(p->vts, pos, &cell)) {
                ch = cell.chars[0] ? cell.chars[0] : ' ';
                reverse = cell.attrs.reverse;
                col_rgb(p, &cell.fg, 1, &fr, &fg, &fb);
                col_rgb(p, &cell.bg, 0, &br, &bg, &bb);
            }
            if (reverse ^ is_cursor) {   /* swap fg/bg for reverse video / block cursor */
                uint8_t t;
                t=fr; fr=br; br=t; t=fg; fg=bg; bg=t; t=fb; fb=bb; bb=t;
            }
            /* glyph bitmap (ASCII 0x20..0x7e; other codepoints render as blank on their bg) */
            const uint8_t *gl = (ch >= 0x20 && ch <= 0x7e) ? FONT6X8[ch - 0x20] : NULL;
            int x0 = cc * p->cellw, y0 = cr * p->cellh;
            for (int gy = 0; gy < BSDR_FONT_H; gy++) {
                uint8_t bits = gl ? gl[gy] : 0;
                for (int sy = 0; sy < S; sy++) {
                    int py = y0 + gy * S + sy;
                    if (py < 0 || py >= h) continue;
                    uint8_t *row = bgr0 + (size_t)py * w * 4;
                    for (int gx = 0; gx < BSDR_FONT_W; gx++) {
                        int on = (bits >> gx) & 1;
                        uint8_t rr = on ? fr : br, gg = on ? fg : bg, bb2 = on ? fb : bb;
                        int px = x0 + gx * S;
                        for (int sx = 0; sx < S; sx++) {
                            int xx = px + sx;
                            if (xx < 0 || xx >= w) continue;
                            uint8_t *pix = row + (size_t)xx * 4;
                            pix[0] = bb2; pix[1] = gg; pix[2] = rr; pix[3] = 0;   /* BGR0 */
                        }
                    }
                }
            }
        }
    }
    bsdr_mutex_unlock(p->lock);
    /* Pad any area beyond the cell grid (when w/h > px_w/px_h) with black so the frame is defined. */
    for (int y = p->px_h; y < h; y++) memset(bgr0 + (size_t)y * w * 4, 0, (size_t)w * 4);
    if (p->px_w < w) for (int y = 0; y < p->px_h && y < h; y++) memset(bgr0 + (size_t)y * w * 4 + (size_t)p->px_w * 4, 0, (size_t)(w - p->px_w) * 4);
    return 0;
}

/* ---- input translation ---------------------------------------------------------------------- */
static VTermKey vk_to_vtermkey(uint16_t vk) {
    switch (vk) {
        case 0x08: return VTERM_KEY_BACKSPACE; case 0x09: return VTERM_KEY_TAB;
        case 0x0D: return VTERM_KEY_ENTER;     case 0x1B: return VTERM_KEY_ESCAPE;
        case 0x21: return VTERM_KEY_PAGEUP;    case 0x22: return VTERM_KEY_PAGEDOWN;
        case 0x23: return VTERM_KEY_END;       case 0x24: return VTERM_KEY_HOME;
        case 0x25: return VTERM_KEY_LEFT;      case 0x26: return VTERM_KEY_UP;
        case 0x27: return VTERM_KEY_RIGHT;     case 0x28: return VTERM_KEY_DOWN;
        case 0x2D: return VTERM_KEY_INS;       case 0x2E: return VTERM_KEY_DEL;
        default: break;
    }
    if (vk >= 0x70 && vk <= 0x7B) return VTERM_KEY_FUNCTION(1 + (vk - 0x70));   /* F1..F12 */
    return VTERM_KEY_NONE;
}

static int vk_is_modifier(uint16_t vk, int *bit) {
    switch (vk) {
        case 0x10: case 0xA0: case 0xA1: *bit = VTERM_MOD_SHIFT; return 1;
        case 0x11: case 0xA2: case 0xA3: *bit = VTERM_MOD_CTRL;  return 1;
        case 0x12: case 0xA4: case 0xA5: *bit = VTERM_MOD_ALT;   return 1;
        default: return 0;
    }
}

void bsdr_term_pty_input(struct bsdr_term_pty *p, const bsdr_input_event *ev) {
    if (!p || p->dead) return;
    bsdr_mutex_lock(p->lock);
    if (p->mod && mono_ms() - p->latch_ms > 1500) p->mod = 0;   /* release a stale latch */
    switch (ev->kind) {
        case BSDR_EV_KEY: {
            uint16_t v = ev->u.key.value;
            int mbit = 0;
            if (ev->u.key.is_vk && vk_is_modifier(v, &mbit)) {
                if (ev->u.key.down) { p->mod |= mbit; p->latch_ms = mono_ms(); }   /* latch; swallow up */
                break;
            }
            if (!ev->u.key.down) { break; }   /* terminals act on key-down; drop the up */
            VTermModifier mod = (VTermModifier)p->mod;
            if (ev->u.key.is_vk) {
                VTermKey k = vk_to_vtermkey(v);
                if (k != VTERM_KEY_NONE) vterm_keyboard_key(p->vt, k, mod);
                else if (v == 0x20) vterm_keyboard_unichar(p->vt, ' ', mod);
            } else {
                uint32_t cp = v;
                /* An uppercase letter / shifted symbol from the soft keyboard already carries the
                 * shifted glyph; don't also send MOD_SHIFT (would double-shift in the app). */
                VTermModifier m = mod;
                if (cp >= 'A' && cp <= 'Z') m = (VTermModifier)(m & ~VTERM_MOD_SHIFT);
                vterm_keyboard_unichar(p->vt, cp, m);
            }
            p->mod = 0;   /* one-shot: latched modifiers released after the key */
            break;
        }
        case BSDR_EV_MOVE_ABS: {
            int col = (int)(ev->u.move_abs.x * p->cols); if (col < 0) col = 0; if (col >= p->cols) col = p->cols - 1;
            int row = (int)(ev->u.move_abs.y * p->rows); if (row < 0) row = 0; if (row >= p->rows) row = p->rows - 1;
            vterm_mouse_move(p->vt, row, col, (VTermModifier)p->mod);   /* no-op unless the app enabled mouse */
            break;
        }
        case BSDR_EV_BUTTON: {
            int b = ev->u.button.button == BSDR_BTN_RIGHT ? 3 : ev->u.button.button == BSDR_BTN_MIDDLE ? 2 : 1;
            vterm_mouse_button(p->vt, b, ev->u.button.down ? true : false, (VTermModifier)p->mod);
            break;
        }
        case BSDR_EV_SCROLL: {
            if (ev->u.scroll.dy) {
                int b = ev->u.scroll.dy > 0 ? 4 : 5, n = ev->u.scroll.dy > 0 ? ev->u.scroll.dy : -ev->u.scroll.dy;
                for (int i = 0; i < n && i < 8; i++) {
                    vterm_mouse_button(p->vt, b, true, (VTermModifier)p->mod);
                    vterm_mouse_button(p->vt, b, false, (VTermModifier)p->mod);
                }
            }
            break;
        }
        default: break;
    }
    bsdr_mutex_unlock(p->lock);
}

int bsdr_term_pty_dead(struct bsdr_term_pty *p) {
    if (!p) return 1;
    if (p->dead) return 1;
    if (p->child > 0 && waitpid(p->child, NULL, WNOHANG) > 0) { p->dead = 1; return 1; }
    return 0;
}

void bsdr_term_pty_stop(struct bsdr_term_pty *p) {
    if (!p) return;
    p->stop = 1;
    if (p->child > 0) kill(p->child, SIGHUP);
    if (p->master >= 0) { close(p->master); p->master = -1; }   /* unblocks the reader's read() */
    if (p->reader) bsdr_thread_join(p->reader);
    if (p->child > 0) { kill(p->child, SIGKILL); waitpid(p->child, NULL, 0); }
    if (p->vt) vterm_free(p->vt);
    if (p->lock) bsdr_mutex_free(p->lock);
    free(p);
}

#endif /* linux && BSDR_HAVE_VTERM */
