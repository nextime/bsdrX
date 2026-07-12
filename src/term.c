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
/* Terminal source backends. See include/bsdr/term.h.
 *  - XVFB : spawn a private Xvfb + xterm; the agent captures it with x11grab and we inject the
 *           Quest's mouse/keyboard into that display via XTEST (Xvfb, unlike a real Xorg, has no
 *           evdev backend, so uinput can't reach it — XTEST is the injection channel).
 *  - PTY  : an in-process libvterm terminal rendered straight to video (implemented in term_pty.c);
 *           this file dispatches to it. */
#include "bsdr/term.h"
#include "bsdr/events.h"
#include "bsdr/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__linux__) && !defined(BSDR_PLATFORM_ANDROID)

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#if defined(BSDR_HAVE_XTEST)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#endif

/* ---- PTY backend hooks (term_pty.c). Weak-ish: compiled only when built with libvterm. --------- */
#if defined(BSDR_HAVE_VTERM)
struct bsdr_term_pty;
struct bsdr_term_pty *bsdr_term_pty_start(const char *cmd, int cols, int rows);
void bsdr_term_pty_size(struct bsdr_term_pty *p, int *w, int *h);
int  bsdr_term_pty_render(struct bsdr_term_pty *p, uint8_t *bgr0, int w, int h);
void bsdr_term_pty_input(struct bsdr_term_pty *p, const bsdr_input_event *ev);
int  bsdr_term_pty_dead(struct bsdr_term_pty *p);
void bsdr_term_pty_stop(struct bsdr_term_pty *p);
#endif

struct bsdr_term {
    int backend;                 /* BSDR_TERM_PTY / BSDR_TERM_XVFB */
    /* --- XVFB --- */
    pid_t xpid;                  /* the Xvfb process */
    pid_t tpid;                  /* the terminal (xterm) process */
    char display[16];            /* ":99" */
    int screen_w, screen_h;
#if defined(BSDR_HAVE_XTEST)
    Display *dpy;                /* XTEST injection connection to `display` */
    int latched[8]; int n_latched;   /* sticky modifiers (Quest sends them as taps) */
    unsigned long long latch_ms;
#endif
    /* --- PTY --- */
#if defined(BSDR_HAVE_VTERM)
    struct bsdr_term_pty *pty;
#endif
};

/* The XVFB backend requires XTEST for injection (Xvfb has no evdev backend), so these helpers — and
 * the whole spawn/capture machinery — are only compiled when it's available. Without XTEST,
 * xvfb_start() below is a stub, so guarding them here avoids -Wunused-function in that build. */
#if defined(BSDR_HAVE_XTEST)
static unsigned long long mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
}

/* ===================== XVFB backend ============================================================= */

/* Pick a free X display number: neither the socket nor the lock file present. */
static int pick_display(void) {
    for (int n = 90; n < 120; n++) {
        char sock[64], lock[64];
        snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n);
        snprintf(lock, sizeof lock, "/tmp/.X%d-lock", n);
        struct stat st;
        if (stat(sock, &st) != 0 && stat(lock, &st) != 0) return n;
    }
    return -1;
}

/* fork/exec argv[0] with argv; returns child pid or -1. Child detaches its stdio to /dev/null. */
static pid_t spawn(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); if (devnull > 2) close(devnull); }
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

/* Wait up to timeout_ms for the X socket of display :n to appear (server ready). */
static int wait_x_ready(int n, int timeout_ms) {
    char sock[64];
    snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n);
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        struct stat st;
        if (stat(sock, &st) == 0) { struct timespec ts = {0, 100*1000*1000}; nanosleep(&ts, NULL); return 1; }
        struct timespec ts = {0, 50*1000*1000}; nanosleep(&ts, NULL);
    }
    return 0;
}
#endif /* BSDR_HAVE_XTEST (XVFB spawn helpers) */

static int xvfb_start(struct bsdr_term *t, const bsdr_term_config *cfg) {
#if !defined(BSDR_HAVE_XTEST)
    (void)t; (void)cfg;
    BSDR_ERROR("bsdr.term", "xvfb terminal needs the XTEST build (libXtst); rebuild with it, or use --terminal=pty");
    return -1;
#else
    int n = pick_display();
    if (n < 0) { BSDR_ERROR("bsdr.term", "no free X display in :90..:119"); return -1; }
    int w = cfg->width  > 0 ? cfg->width  : 1280;
    int h = cfg->height > 0 ? cfg->height : 720;
    t->screen_w = w; t->screen_h = h;
    snprintf(t->display, sizeof t->display, ":%d", n);

    char geom[32]; snprintf(geom, sizeof geom, "%dx%dx24", w, h);
    char *xvfb_argv[] = { (char*)"Xvfb", t->display, (char*)"-screen", (char*)"0", geom,
                          (char*)"-nolisten", (char*)"tcp", (char*)"-noreset", NULL };
    t->xpid = spawn(xvfb_argv);
    if (t->xpid < 0) { BSDR_ERROR("bsdr.term", "cannot spawn Xvfb (is it installed? apt/dnf install xvfb)"); return -1; }
    if (!wait_x_ready(n, 5000)) {
        BSDR_ERROR("bsdr.term", "Xvfb %s did not come up (install xvfb; check it runs)", t->display);
        kill(t->xpid, SIGTERM); return -1;
    }

    /* xterm sized to fill the virtual screen (no WM here, so give an explicit char geometry). A
     * 14px mono cell is ~8x17 px; compute cols/rows to cover w x h. */
    int cols = w / 8; if (cols < 20) cols = 20; if (cols > 400) cols = 400;
    int rows = h / 17; if (rows < 6) rows = 6; if (rows > 200) rows = 200;
    char xgeom[32]; snprintf(xgeom, sizeof xgeom, "%dx%d+0+0", cols, rows);

    const char *shell = cfg->cmd && cfg->cmd[0] ? NULL : (getenv("SHELL") ? getenv("SHELL") : "/bin/bash");
    /* Build: xterm -display :N -geometry CxR -fa "DejaVu Sans Mono" -fs 14 -bg black -fg white
     *              +sb -e  <shell | /bin/sh -c CMD> */
    char *base[] = {
        (char*)"xterm", (char*)"-display", t->display, (char*)"-geometry", xgeom,
        (char*)"-fa", (char*)"DejaVu Sans Mono", (char*)"-fs", (char*)"14",
        (char*)"-bg", (char*)"black", (char*)"-fg", (char*)"white", (char*)"+sb",
        (char*)"-e", NULL, NULL, NULL, NULL };
    int bi = 15;   /* index of the "-e" slot's argument */
    if (shell) { base[bi] = (char*)shell; base[bi+1] = NULL; }
    else       { base[bi] = (char*)"/bin/sh"; base[bi+1] = (char*)"-c"; base[bi+2] = (char*)cfg->cmd; base[bi+3] = NULL; }
    t->tpid = spawn(base);
    if (t->tpid < 0) {
        BSDR_ERROR("bsdr.term", "cannot spawn xterm (is it installed? apt/dnf install xterm)");
        kill(t->xpid, SIGTERM); return -1;
    }

    /* Injection connection to the private display. */
    t->dpy = XOpenDisplay(t->display);
    if (!t->dpy) BSDR_WARN("bsdr.term", "XTEST: cannot open %s for injection (keyboard/mouse will be dead)", t->display);
    int ev=0, er=0, mj=0, mn=0;
    if (t->dpy && !XTestQueryExtension(t->dpy, &ev, &er, &mj, &mn))
        BSDR_WARN("bsdr.term", "XTEST extension missing on %s; input injection unavailable", t->display);
    BSDR_INFO("bsdr.term", "xvfb terminal up: Xvfb %s %dx%d + xterm(%dx%d) -> x11grab, XTEST input", t->display, w, h, cols, rows);
    return 0;
#endif
}

#if defined(BSDR_HAVE_XTEST)
/* Map a bsdrX key event to an X KeySym. Printable ASCII maps to itself (Latin-1). */
static KeySym vk_to_keysym(uint16_t vk) {
    switch (vk) {
        case 0x08: return XK_BackSpace; case 0x09: return XK_Tab;    case 0x0D: return XK_Return;
        case 0x1B: return XK_Escape;    case 0x20: return XK_space;
        case 0x21: return XK_Prior;     case 0x22: return XK_Next;
        case 0x23: return XK_End;       case 0x24: return XK_Home;
        case 0x25: return XK_Left;      case 0x26: return XK_Up;
        case 0x27: return XK_Right;     case 0x28: return XK_Down;
        case 0x2D: return XK_Insert;    case 0x2E: return XK_Delete;
        case 0x10: case 0xA0: return XK_Shift_L;   case 0xA1: return XK_Shift_R;
        case 0x11: case 0xA2: return XK_Control_L; case 0xA3: return XK_Control_R;
        case 0x12: case 0xA4: return XK_Alt_L;     case 0xA5: return XK_Alt_R;
        case 0x5B: return XK_Super_L;
        default: break;
    }
    if (vk >= 0x70 && vk <= 0x7B) return XK_F1 + (vk - 0x70);   /* F1..F12 */
    if (vk >= 'A' && vk <= 'Z') return XK_a + (vk - 'A');        /* letters: unshifted keysym */
    if (vk >= '0' && vk <= '9') return XK_0 + (vk - '0');
    return NoSymbol;
}

static int keysym_is_modifier(KeySym ks) {
    return ks == XK_Shift_L || ks == XK_Shift_R || ks == XK_Control_L || ks == XK_Control_R ||
           ks == XK_Alt_L   || ks == XK_Alt_R   || ks == XK_Super_L   || ks == XK_Super_R;
}

/* Resolve a keysym to a keycode, borrowing a spare keycode (remap) for symbols not in the layout
 * (accented / non-US). Returns the keycode and, if borrowed, sets *borrowed so the caller unmaps it. */
static KeyCode keysym_to_keycode(Display *dpy, KeySym ks, int *borrowed) {
    *borrowed = 0;
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (kc) return kc;
    int min_kc = 8, max_kc = 255, per = 0;
    XDisplayKeycodes(dpy, &min_kc, &max_kc);
    KeySym *map = XGetKeyboardMapping(dpy, min_kc, max_kc - min_kc + 1, &per);
    if (!map || per < 1) { if (map) XFree(map); return 0; }
    int spare = -1;
    for (int c = max_kc; c >= min_kc; c--) {
        int used = 0;
        for (int j = 0; j < per; j++) if (map[(c - min_kc) * per + j]) { used = 1; break; }
        if (!used) { spare = c; break; }
    }
    XFree(map);
    if (spare < 0) return 0;
    KeySym two[2] = { ks, ks };
    XChangeKeyboardMapping(dpy, spare, 2, two, 1);
    XSync(dpy, False);
    *borrowed = 1;
    return (KeyCode)spare;
}

static void xvfb_release_latched(struct bsdr_term *t) {
    for (int i = t->n_latched - 1; i >= 0; i--)
        XTestFakeKeyEvent(t->dpy, (KeyCode)t->latched[i], False, 0);
    if (t->n_latched) XSync(t->dpy, False);
    t->n_latched = 0;
}

static void xvfb_key(struct bsdr_term *t, const bsdr_input_event *ev) {
    KeySym ks;
    uint16_t v = ev->u.key.value;
    if (ev->u.key.is_vk) ks = vk_to_keysym(v);
    else ks = (v >= 0x20 && v < 0x7f) ? (KeySym)v : (KeySym)(0x01000000u | v);   /* ASCII -> Latin1; else Unicode keysym */
    if (ks == NoSymbol) return;

    if (keysym_is_modifier(ks)) {
        /* Quest sends a modifier as a momentary tap; latch it across the next key (like uinput). */
        if (ev->u.key.down) {
            int borrowed; KeyCode kc = keysym_to_keycode(t->dpy, ks, &borrowed);
            if (!kc) return;
            for (int i = 0; i < t->n_latched; i++) if (t->latched[i] == kc) { t->latch_ms = mono_ms(); return; }
            if (t->n_latched < 8) {
                XTestFakeKeyEvent(t->dpy, kc, True, 0); XSync(t->dpy, False);
                t->latched[t->n_latched++] = kc; t->latch_ms = mono_ms();
            }
        }
        return;   /* swallow the up */
    }
    int borrowed; KeyCode kc = keysym_to_keycode(t->dpy, ks, &borrowed);
    if (!kc) return;
    /* Shift for a base-letter keysym when the char was uppercase / a shifted symbol: XTEST reads the
     * live keymap, so for ASCII we press Shift ourselves when needed. */
    int need_shift = (!ev->u.key.is_vk && ((v >= 'A' && v <= 'Z') ||
                      strchr("~!@#$%^&*()_+{}|:\"<>?", (int)v) != NULL));
    if (need_shift && ev->u.key.down) XTestFakeKeyEvent(t->dpy, XKeysymToKeycode(t->dpy, XK_Shift_L), True, 0);
    XTestFakeKeyEvent(t->dpy, kc, ev->u.key.down ? True : False, 0);
    if (need_shift && !ev->u.key.down) XTestFakeKeyEvent(t->dpy, XKeysymToKeycode(t->dpy, XK_Shift_L), False, 0);
    XSync(t->dpy, False);
    if (borrowed) {   /* release the borrowed keycode after use */
        KeySym none[2] = { NoSymbol, NoSymbol };
        XChangeKeyboardMapping(t->dpy, kc, 2, none, 1); XSync(t->dpy, False);
    }
    if (!ev->u.key.down) xvfb_release_latched(t);   /* one-shot: drop modifiers after the key lifts */
}

static void xvfb_input(struct bsdr_term *t, const bsdr_input_event *ev) {
    if (!t->dpy) return;
    /* Auto-release a latch no key consumed (stray/duplicate modifier tap) after 1.5s. */
    if (t->n_latched && mono_ms() - t->latch_ms > 1500) xvfb_release_latched(t);
    switch (ev->kind) {
        case BSDR_EV_MOVE_ABS:
            XTestFakeMotionEvent(t->dpy, DefaultScreen(t->dpy),
                                 (int)(ev->u.move_abs.x * t->screen_w),
                                 (int)(ev->u.move_abs.y * t->screen_h), 0);
            XSync(t->dpy, False);
            break;
        case BSDR_EV_MOVE_REL:
            XTestFakeRelativeMotionEvent(t->dpy, ev->u.move_rel.dx, ev->u.move_rel.dy, 0);
            XSync(t->dpy, False);
            break;
        case BSDR_EV_BUTTON: {
            int b = ev->u.button.button == BSDR_BTN_RIGHT ? 3 :
                    ev->u.button.button == BSDR_BTN_MIDDLE ? 2 : 1;
            XTestFakeButtonEvent(t->dpy, b, ev->u.button.down ? True : False, 0);
            XSync(t->dpy, False);
            break;
        }
        case BSDR_EV_SCROLL:
            if (ev->u.scroll.dy) {   /* button 4 = up, 5 = down (one click per notch) */
                int b = ev->u.scroll.dy > 0 ? 4 : 5, n = ev->u.scroll.dy > 0 ? ev->u.scroll.dy : -ev->u.scroll.dy;
                for (int i = 0; i < n && i < 10; i++) { XTestFakeButtonEvent(t->dpy, b, True, 0); XTestFakeButtonEvent(t->dpy, b, False, 0); }
                XSync(t->dpy, False);
            }
            if (ev->u.scroll.dx) {
                int b = ev->u.scroll.dx > 0 ? 7 : 6, n = ev->u.scroll.dx > 0 ? ev->u.scroll.dx : -ev->u.scroll.dx;
                for (int i = 0; i < n && i < 10; i++) { XTestFakeButtonEvent(t->dpy, b, True, 0); XTestFakeButtonEvent(t->dpy, b, False, 0); }
                XSync(t->dpy, False);
            }
            break;
        case BSDR_EV_KEY: xvfb_key(t, ev); break;
        default: break;
    }
}
#endif /* BSDR_HAVE_XTEST */

static void xvfb_stop(struct bsdr_term *t) {
#if defined(BSDR_HAVE_XTEST)
    if (t->dpy) { xvfb_release_latched(t); XCloseDisplay(t->dpy); t->dpy = NULL; }
#endif
    if (t->tpid > 0) { kill(t->tpid, SIGTERM); }
    if (t->xpid > 0) { kill(t->xpid, SIGTERM); }
    /* reap (Xvfb exits once its last client is gone; give SIGKILL a moment if needed) */
    for (int i = 0; i < 20; i++) {
        int any = 0;
        if (t->tpid > 0 && waitpid(t->tpid, NULL, WNOHANG) == 0) any = 1;
        if (t->xpid > 0 && waitpid(t->xpid, NULL, WNOHANG) == 0) any = 1;
        if (!any) break;
        struct timespec ts = {0, 50*1000*1000}; nanosleep(&ts, NULL);
        if (i == 10) { if (t->tpid > 0) kill(t->tpid, SIGKILL); if (t->xpid > 0) kill(t->xpid, SIGKILL); }
    }
    t->tpid = t->xpid = -1;
}

/* ===================== public API ============================================================== */

bsdr_term *bsdr_term_start(const bsdr_term_config *cfg) {
    if (!cfg) return NULL;
    bsdr_term *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->backend = cfg->backend;
    t->xpid = t->tpid = -1;
    if (cfg->backend == BSDR_TERM_XVFB) {
        if (xvfb_start(t, cfg) != 0) { free(t); return NULL; }
        return t;
    }
    /* PTY backend */
#if defined(BSDR_HAVE_VTERM)
    int cols = cfg->cols > 0 ? cfg->cols : 120, rows = cfg->rows > 0 ? cfg->rows : 36;
    t->pty = bsdr_term_pty_start(cfg->cmd, cols, rows);
    if (!t->pty) { free(t); return NULL; }
    return t;
#else
    BSDR_ERROR("bsdr.term", "pty terminal needs the libvterm build; rebuild with it, or use --terminal=xvfb");
    free(t);
    return NULL;
#endif
}

const char *bsdr_term_display(bsdr_term *t) {
    return (t && t->backend == BSDR_TERM_XVFB && t->display[0]) ? t->display : NULL;
}

int bsdr_term_is_pty(bsdr_term *t) { return t && t->backend == BSDR_TERM_PTY; }

void bsdr_term_size(bsdr_term *t, int *w, int *h) {
#if defined(BSDR_HAVE_VTERM)
    if (t && t->backend == BSDR_TERM_PTY && t->pty) { bsdr_term_pty_size(t->pty, w, h); return; }
#endif
    if (w) *w = t ? t->screen_w : 0;
    if (h) *h = t ? t->screen_h : 0;
}

int bsdr_term_render(void *term, uint8_t *bgr0, int w, int h) {
    bsdr_term *t = (bsdr_term *)term;
#if defined(BSDR_HAVE_VTERM)
    if (t && t->backend == BSDR_TERM_PTY && t->pty) return bsdr_term_pty_render(t->pty, bgr0, w, h);
#endif
    (void)t; (void)bgr0; (void)w; (void)h;
    return -1;
}

void bsdr_term_input(bsdr_term *t, const bsdr_input_event *ev) {
    if (!t || !ev) return;
    if (t->backend == BSDR_TERM_XVFB) {
#if defined(BSDR_HAVE_XTEST)
        xvfb_input(t, ev);
#endif
        return;
    }
#if defined(BSDR_HAVE_VTERM)
    if (t->pty) bsdr_term_pty_input(t->pty, ev);
#endif
}

int bsdr_term_dead(bsdr_term *t) {
    if (!t) return 1;
    if (t->backend == BSDR_TERM_XVFB) {
        if (t->tpid > 0 && waitpid(t->tpid, NULL, WNOHANG) > 0) { t->tpid = -1; return 1; }
        return t->tpid <= 0;
    }
#if defined(BSDR_HAVE_VTERM)
    return t->pty ? bsdr_term_pty_dead(t->pty) : 1;
#else
    return 1;
#endif
}

void bsdr_term_stop(bsdr_term *t) {
    if (!t) return;
    if (t->backend == BSDR_TERM_XVFB) xvfb_stop(t);
#if defined(BSDR_HAVE_VTERM)
    else if (t->pty) { bsdr_term_pty_stop(t->pty); t->pty = NULL; }
#endif
    free(t);
}

#else  /* non-Linux (or Android): terminal source unsupported */

bsdr_term *bsdr_term_start(const bsdr_term_config *cfg) { (void)cfg; return NULL; }
const char *bsdr_term_display(bsdr_term *t) { (void)t; return NULL; }
int bsdr_term_is_pty(bsdr_term *t) { (void)t; return 0; }
void bsdr_term_size(bsdr_term *t, int *w, int *h) { (void)t; if (w) *w = 0; if (h) *h = 0; }
int bsdr_term_render(void *term, uint8_t *bgr0, int w, int h) { (void)term; (void)bgr0; (void)w; (void)h; return -1; }
void bsdr_term_input(bsdr_term *t, const bsdr_input_event *ev) { (void)t; (void)ev; }
int bsdr_term_dead(bsdr_term *t) { (void)t; return 1; }
void bsdr_term_stop(bsdr_term *t) { (void)t; }

#endif
