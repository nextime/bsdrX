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
/* Linux input injection via uinput — the Linux analog of SendInput + ViGEmBus.
 * Creates virtual mouse / absolute-pointer / keyboard / gamepad devices. Falls
 * back to a logging stub if /dev/uinput is unavailable. */
#include "bsdr/inject.h"
#include "bsdr/log.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <linux/uinput.h>

struct bsdr_injector {
    int mouse, absdev, kbd, pad;
    int touch;                     /* multitouch (ABS_MT) device for touch pointer mode */
    int touch_x, touch_y;          /* last pointer position (device units), for a touch-down at point */
    bool touching;                 /* a finger is currently down (touch mode) */
    int screen_w, screen_h;
    bool ok;
    /* Sticky modifiers: the Quest's soft keyboard sends a modifier (Ctrl/Shift/
     * Alt/Meta) as a momentary tap (down+up) and then sends the next key on its
     * own — it expects the host to LATCH the modifier and hold it across the
     * following key. We mirror that: a modifier tap is held until the next
     * non-modifier key is released, then all latched modifiers are let go. */
    int latched[8];
    int n_latched;
    unsigned long long latch_ms;   /* when the latch set was armed (for the timeout release) */
    int remap_arrow;               /* KEY_UP/KEY_DOWN whose down we remapped to paging (0 = none),
                                    * so the matching key-up remaps to the same paging key. */
    unsigned long long remap_ms;   /* when the paging remap was armed (for the timeout release) */
};

#define BSDR_LATCH_TIMEOUT_MS 1500   /* a sticky modifier no key consumed is auto-released */

/* Pointer mode (process-global; the injector is per-session). 0 = mouse, 1 = real touch. */
static int g_touch_mode = 0;
void bsdr_injector_touch_mode(int on) { g_touch_mode = on ? 1 : 0; }

#if defined(BSDR_HAVE_XTEST)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
/* Type a Unicode character uinput can't (non-ASCII from the headset's soft keyboard) via X11 XTEST:
 * temporarily map a spare keycode to the Unicode keysym (0x01000000|cp), fake a press, then clear it.
 * X11/XWayland only. Returns 1 if injected, 0 otherwise (caller then logs it unmapped). */
static int x11_type_unicode(uint16_t ch) {
    static Display *dpy = NULL;
    static int tried = 0;
    if (!dpy && !tried) { tried = 1; dpy = XOpenDisplay(NULL); }
    if (!dpy) return 0;
    int min_kc = 8, max_kc = 255, per = 0;
    XDisplayKeycodes(dpy, &min_kc, &max_kc);
    KeySym *map = XGetKeyboardMapping(dpy, min_kc, max_kc - min_kc + 1, &per);
    if (!map || per < 1) { if (map) XFree(map); return 0; }
    int spare = -1;
    for (int kc = max_kc; kc >= min_kc; kc--) {
        int used = 0;
        for (int j = 0; j < per; j++) if (map[(kc - min_kc) * per + j]) { used = 1; break; }
        if (!used) { spare = kc; break; }
    }
    XFree(map);
    if (spare < 0) return 0;
    KeySym ks = (KeySym)(0x01000000u | (unsigned)ch);
    KeySym two[2] = { ks, ks };
    XChangeKeyboardMapping(dpy, spare, 2, two, 1);
    XSync(dpy, False);
    XTestFakeKeyEvent(dpy, spare, True, 0);
    XTestFakeKeyEvent(dpy, spare, False, 0);
    XSync(dpy, False);
    KeySym none[2] = { 0, 0 };
    XChangeKeyboardMapping(dpy, spare, 2, none, 1);   /* release the borrowed keycode */
    XSync(dpy, False);
    return 1;
}
#else
static int x11_type_unicode(uint16_t ch) { (void)ch; return 0; }
#endif

static unsigned long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
}

static bool is_modifier_code(int code) {
    return code == KEY_LEFTCTRL  || code == KEY_RIGHTCTRL  ||
           code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
           code == KEY_LEFTALT   || code == KEY_RIGHTALT   ||
           code == KEY_LEFTMETA  || code == KEY_RIGHTMETA;
}

static bool ctrl_is_latched(const struct bsdr_injector *inj) {
    for (int i = 0; i < inj->n_latched; i++)
        if (inj->latched[i] == KEY_LEFTCTRL || inj->latched[i] == KEY_RIGHTCTRL) return true;
    return false;
}

static void emit(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = (unsigned short)type;
    ie.code = (unsigned short)code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0) { /* best-effort */ }
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static int make_device(const char *name,
                       const int *ev_bits, int n_ev,
                       const int *keys, int n_keys,
                       const int *rels, int n_rels,
                       const struct uinput_abs_setup *abs, int n_abs,
                       int prop) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    for (int i = 0; i < n_ev; i++) ioctl(fd, UI_SET_EVBIT, ev_bits[i]);
    for (int i = 0; i < n_keys; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    for (int i = 0; i < n_rels; i++) ioctl(fd, UI_SET_RELBIT, rels[i]);
    for (int i = 0; i < n_abs; i++) {
        ioctl(fd, UI_SET_ABSBIT, abs[i].code);
        ioctl(fd, UI_ABS_SETUP, &abs[i]);
    }
    /* INPUT_PROP_POINTER => userspace (libinput/X) treats an ABS device as an
     * absolute MOUSE, not a touchscreen — so the cursor tracks & clicks work. */
    if (prop >= 0) ioctl(fd, UI_SET_PROPBIT, prop);
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1234;
    us.id.product = 0x5678;
    snprintf(us.name, sizeof(us.name), "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- key maps (subset; extend as captures reveal usage) -------------------*/
static int vk_to_key(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        static const int a[] = {
            KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,
            KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,
            KEY_W,KEY_X,KEY_Y,KEY_Z };
        return a[vk - 'A'];
    }
    if (vk >= '0' && vk <= '9') {
        static const int d[] = { KEY_0,KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,
                                 KEY_7,KEY_8,KEY_9 };
        return d[vk - '0'];
    }
    if (vk >= 0x70 && vk <= 0x7B) {  /* F1..F12 */
        static const int f[] = { KEY_F1,KEY_F2,KEY_F3,KEY_F4,KEY_F5,KEY_F6,
            KEY_F7,KEY_F8,KEY_F9,KEY_F10,KEY_F11,KEY_F12 };
        return f[vk - 0x70];
    }
    switch (vk) {
        case 0x08: return KEY_BACKSPACE;
        case 0x09: return KEY_TAB;
        case 0x0D: return KEY_ENTER;
        case 0x10: case 0xA0: return KEY_LEFTSHIFT;
        case 0xA1: return KEY_RIGHTSHIFT;
        case 0x11: case 0xA2: return KEY_LEFTCTRL;
        case 0xA3: return KEY_RIGHTCTRL;
        case 0x12: case 0xA4: return KEY_LEFTALT;
        case 0xA5: return KEY_RIGHTALT;
        case 0x1B: return KEY_ESC;
        case 0x20: return KEY_SPACE;
        case 0x21: return KEY_PAGEUP;
        case 0x22: return KEY_PAGEDOWN;
        case 0x23: return KEY_END;
        case 0x24: return KEY_HOME;
        case 0x25: return KEY_LEFT;
        case 0x26: return KEY_UP;
        case 0x27: return KEY_RIGHT;
        case 0x28: return KEY_DOWN;
        case 0x2D: return KEY_INSERT;
        case 0x2E: return KEY_DELETE;
        case 0x5B: return KEY_LEFTMETA;
        case 0xBA: return KEY_SEMICOLON;
        case 0xBB: return KEY_EQUAL;
        case 0xBC: return KEY_COMMA;
        case 0xBD: return KEY_MINUS;
        case 0xBE: return KEY_DOT;
        case 0xBF: return KEY_SLASH;
        case 0xC0: return KEY_GRAVE;
        case 0xDB: return KEY_LEFTBRACE;
        case 0xDC: return KEY_BACKSLASH;
        case 0xDD: return KEY_RIGHTBRACE;
        case 0xDE: return KEY_APOSTROPHE;
        default: return -1;
    }
}

/* ASCII char -> KEY_ + whether Shift is required (US layout). */
static int ascii_to_key(uint16_t ch, bool *shift) {
    *shift = false;
    if (ch >= 'a' && ch <= 'z') return vk_to_key((uint16_t)(ch - 'a' + 'A'));
    if (ch >= 'A' && ch <= 'Z') { *shift = true; return vk_to_key(ch); }
    if (ch >= '0' && ch <= '9') return vk_to_key(ch);
    switch (ch) {
        case ' ': return KEY_SPACE;
        case '\n': case '\r': return KEY_ENTER;
        case '\t': return KEY_TAB;
        case '-': return KEY_MINUS;          case '=': return KEY_EQUAL;
        case '[': return KEY_LEFTBRACE;      case ']': return KEY_RIGHTBRACE;
        case '\\': return KEY_BACKSLASH;     case ';': return KEY_SEMICOLON;
        case '\'': return KEY_APOSTROPHE;    case '`': return KEY_GRAVE;
        case ',': return KEY_COMMA;          case '.': return KEY_DOT;
        case '/': return KEY_SLASH;
        case '!': *shift = true; return KEY_1;   case '@': *shift = true; return KEY_2;
        case '#': *shift = true; return KEY_3;   case '$': *shift = true; return KEY_4;
        case '%': *shift = true; return KEY_5;   case '^': *shift = true; return KEY_6;
        case '&': *shift = true; return KEY_7;   case '*': *shift = true; return KEY_8;
        case '(': *shift = true; return KEY_9;   case ')': *shift = true; return KEY_0;
        case '_': *shift = true; return KEY_MINUS;     case '+': *shift = true; return KEY_EQUAL;
        case '{': *shift = true; return KEY_LEFTBRACE; case '}': *shift = true; return KEY_RIGHTBRACE;
        case '|': *shift = true; return KEY_BACKSLASH; case ':': *shift = true; return KEY_SEMICOLON;
        case '"': *shift = true; return KEY_APOSTROPHE; case '~': *shift = true; return KEY_GRAVE;
        case '<': *shift = true; return KEY_COMMA;      case '>': *shift = true; return KEY_DOT;
        case '?': *shift = true; return KEY_SLASH;
        default: return -1;
    }
}

static int mouse_btn_code(bsdr_mouse_button b) {
    switch (b) {
        case BSDR_BTN_LEFT: return BTN_LEFT;
        case BSDR_BTN_RIGHT: return BTN_RIGHT;
        case BSDR_BTN_MIDDLE: return BTN_MIDDLE;
        case BSDR_BTN_X1: return BTN_SIDE;
        case BSDR_BTN_X2: return BTN_EXTRA;
        default: return BTN_LEFT;
    }
}

/* ---- crash-safe cleanup --------------------------------------------------- */
/* A fatal signal (SIGSEGV/SIGABRT/…) bypasses bsdr_injector_destroy, so the
 * virtual devices would die by an abrupt fd-close — which can leave a button/key
 * latched and even wedge the real (I2C-HID) touchpad. We mirror the live fds into
 * file-scope slots and, on a fatal signal, release every button/key and
 * orderly-destroy the devices using only async-signal-safe calls. */
static volatile sig_atomic_t g_clean_mouse = -1, g_clean_abs = -1,
                             g_clean_kbd = -1, g_clean_pad = -1;
static volatile sig_atomic_t g_crash_handlers = 0;

static void release_pressed(int fd_mouse, int fd_abs, int fd_kbd, int fd_pad) {
    static const int mbtn[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
    static const int abtn[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    static const int pbtn[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
                                BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
    if (fd_mouse >= 0) {
        for (size_t i = 0; i < sizeof(mbtn)/sizeof(mbtn[0]); i++) emit(fd_mouse, EV_KEY, mbtn[i], 0);
        syn(fd_mouse);
    }
    if (fd_abs >= 0) {
        for (size_t i = 0; i < sizeof(abtn)/sizeof(abtn[0]); i++) emit(fd_abs, EV_KEY, abtn[i], 0);
        syn(fd_abs);
    }
    if (fd_kbd >= 0) {
        for (int k = KEY_ESC; k <= KEY_MICMUTE; k++) emit(fd_kbd, EV_KEY, k, 0);
        syn(fd_kbd);
    }
    if (fd_pad >= 0) {
        for (size_t i = 0; i < sizeof(pbtn)/sizeof(pbtn[0]); i++) emit(fd_pad, EV_KEY, pbtn[i], 0);
        syn(fd_pad);
    }
}

static void destroy_fds(int fd_mouse, int fd_abs, int fd_kbd, int fd_pad) {
    int fds[] = { fd_mouse, fd_abs, fd_kbd, fd_pad };
    for (int i = 0; i < 4; i++)
        if (fds[i] >= 0) { ioctl(fds[i], UI_DEV_DESTROY); close(fds[i]); }
}

static void crash_cleanup(int sig) {
    static const char msg[] =
        "bsdr.inject: fatal signal, releasing virtual input devices\n";
    ssize_t w = write(STDERR_FILENO, msg, sizeof(msg) - 1); (void)w;
    int m = g_clean_mouse, a = g_clean_abs, k = g_clean_kbd, p = g_clean_pad;
    g_clean_mouse = g_clean_abs = g_clean_kbd = g_clean_pad = -1;
    release_pressed(m, a, k, p);
    destroy_fds(m, a, k, p);
    signal(sig, SIG_DFL);   /* restore default and re-raise so we still core/exit */
    raise(sig);
}

static void install_crash_handlers(void) {
    if (g_crash_handlers) return;
    g_crash_handlers = 1;
    int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_cleanup;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(sigs)/sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

bsdr_injector *bsdr_injector_create(int screen_w, int screen_h) {
    struct bsdr_injector *inj = calloc(1, sizeof(*inj));
    if (!inj) return NULL;
    inj->mouse = inj->absdev = inj->kbd = inj->pad = -1;
    inj->screen_w = screen_w > 0 ? screen_w : 1920;
    inj->screen_h = screen_h > 0 ? screen_h : 1080;

    /* ONE unified pointer (like the Windows host's SendInput): absolute position
     * (ABS_X/Y) + buttons + wheel on a single device, marked INPUT_PROP_POINTER
     * so it behaves as an absolute mouse. Position and clicks MUST come from the
     * same device or the click lands at the wrong/stale spot. REL_X/Y is kept for
     * the occasional relative-move opcode. */
    int ev_ptr[] = { EV_KEY, EV_REL, EV_ABS };
    int pkeys_m[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
    /* wheel only — NOT REL_X/Y: an abs device with REL_X/Y can be misread as a
     * relative mouse and ignore positioning. The Quest sends absolute moves. */
    int prels[] = { REL_WHEEL, REL_HWHEEL };
    struct uinput_abs_setup absx = { .code = ABS_X };
    struct uinput_abs_setup absy = { .code = ABS_Y };
    absx.absinfo.minimum = 0; absx.absinfo.maximum = inj->screen_w;
    absy.absinfo.minimum = 0; absy.absinfo.maximum = inj->screen_h;
    struct uinput_abs_setup absxy[] = { absx, absy };
    inj->mouse = make_device("bsdr-virtual-pointer", ev_ptr, 3, pkeys_m, 5,
                             prels, 2, absxy, 2, INPUT_PROP_POINTER);
    inj->absdev = -1;           /* folded into the unified pointer (inj->mouse) */

    /* Touch pointer mode: a type-B multitouch touchscreen so the headset's tap/drag arrive as REAL
     * touch events (the DE interprets them as touch gestures, not mouse). Created always; used only
     * when the mode is on. INPUT_PROP_DIRECT marks it a touchscreen (absolute, screen-mapped). */
    int ev_tch[] = { EV_KEY, EV_ABS };
    int tkeys[] = { BTN_TOUCH };
    struct uinput_abs_setup tabs[6];
    memset(tabs, 0, sizeof tabs);
    tabs[0].code = ABS_MT_SLOT;        tabs[0].absinfo.maximum = 9;
    tabs[1].code = ABS_MT_TRACKING_ID; tabs[1].absinfo.maximum = 65535;
    tabs[2].code = ABS_MT_POSITION_X;  tabs[2].absinfo.maximum = inj->screen_w;
    tabs[3].code = ABS_MT_POSITION_Y;  tabs[3].absinfo.maximum = inj->screen_h;
    tabs[4].code = ABS_X;              tabs[4].absinfo.maximum = inj->screen_w;
    tabs[5].code = ABS_Y;              tabs[5].absinfo.maximum = inj->screen_h;
    inj->touch = make_device("bsdr-virtual-touch", ev_tch, 2, tkeys, 1, NULL, 0, tabs, 6, INPUT_PROP_DIRECT);

    int ev_kbd[] = { EV_KEY };
    int kkeys[KEY_MAX];
    int nk = 0;
    for (int k = KEY_ESC; k <= KEY_MICMUTE && nk < KEY_MAX; k++) kkeys[nk++] = k;
    inj->kbd = make_device("bsdr-virtual-keyboard", ev_kbd, 1, kkeys, nk, NULL, 0, NULL, 0, -1);

    int ev_pad[] = { EV_KEY, EV_ABS };
    int pkeys[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT,
                    BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
    struct uinput_abs_setup pabs[6];
    memset(pabs, 0, sizeof(pabs));
    int pcodes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ };
    for (int i = 0; i < 6; i++) {
        pabs[i].code = pcodes[i];
        if (i < 4) { pabs[i].absinfo.minimum = -32768; pabs[i].absinfo.maximum = 32767; }
        else { pabs[i].absinfo.minimum = 0; pabs[i].absinfo.maximum = 255; }
    }
    inj->pad = make_device("bsdr-virtual-gamepad", ev_pad, 2, pkeys, 11, NULL, 0, pabs, 6, -1);

    inj->ok = (inj->mouse >= 0 && inj->kbd >= 0);
    if (!inj->ok)
        BSDR_WARN("bsdr.inject", "/dev/uinput unavailable; logging only "
                  "(see README for the udev rule)");
    else
        BSDR_INFO("bsdr.inject", "uinput devices created (mouse/abs/keyboard/gamepad)");

    /* arm crash-safe cleanup for whatever opened */
    g_clean_mouse = inj->mouse; g_clean_abs = inj->absdev;
    g_clean_kbd   = inj->kbd;   g_clean_pad = inj->pad;
    if (inj->mouse >= 0 || inj->absdev >= 0 || inj->kbd >= 0 || inj->pad >= 0)
        install_crash_handlers();
    return inj;
}

static void release_all_latched(struct bsdr_injector *inj) {
    if (inj->n_latched <= 0) return;
    for (int i = inj->n_latched - 1; i >= 0; i--)
        emit(inj->kbd, EV_KEY, inj->latched[i], 0);
    syn(inj->kbd);
    BSDR_INFO("bsdr.inject", "released %d latched modifier(s)", inj->n_latched);
    inj->n_latched = 0;
}

/* Force-release a pending Ctrl+Arrow->paging substitution: emit the paging key-up that the
 * matching arrow key-up would have produced, then disarm. Called when the arrow key-up never
 * arrives (lost/out-of-order) or a new arrow starts before the old one lifted — otherwise the
 * substituted PageUp/PageDown would stay held in the kernel and wedge the keyboard. */
static void release_remap(struct bsdr_injector *inj) {
    if (!inj->remap_arrow) return;
    int paging = (inj->remap_arrow == KEY_UP) ? KEY_PAGEUP : KEY_PAGEDOWN;
    emit(inj->kbd, EV_KEY, paging, 0);
    syn(inj->kbd);
    inj->remap_arrow = 0;
}

static void emit_key(struct bsdr_injector *inj, const bsdr_input_event *ev) {
    int code; bool shift = false;
    if (ev->u.key.is_vk) code = vk_to_key(ev->u.key.value);
    else code = ascii_to_key(ev->u.key.value, &shift);
    BSDR_INFO("bsdr.inject", "KEY %s is_vk=%d value=0x%04x ('%c') -> code=%d",
              ev->u.key.down ? "DOWN" : "UP", ev->u.key.is_vk, ev->u.key.value,
              (ev->u.key.value >= 32 && ev->u.key.value < 127) ? (char)ev->u.key.value : '.', code);
    if (code < 0) {
        /* Character with no US-layout keycode (accented/CJK/symbol from the headset's soft keyboard):
         * inject it as Unicode text via XTEST on the key-DOWN (swallow the UP). Falls back to a log if
         * XTEST is unavailable (no X11 / built without it). */
        if (!ev->u.key.is_vk && ev->u.key.value >= 0x20) {
            if (ev->u.key.down && x11_type_unicode(ev->u.key.value)) return;
            if (!ev->u.key.down) return;
        }
        BSDR_WARN("bsdr.inject", "unmapped key value=0x%04x is_vk=%d (no keycode; Unicode fallback needs X11/XTEST)",
                  ev->u.key.value, ev->u.key.is_vk);
        return;
    }

    /* Modifier key: the Quest sends it as a momentary tap (down+up) and expects the host to
     * LATCH it across the following key. We latch on the down tap and swallow the up; the latch
     * is dropped when the next non-modifier key is released (so Ctrl+C works). A repeated tap of
     * an already-latched modifier just refreshes the timeout (the Quest re-sends Ctrl as two taps),
     * and a timeout (in _handle) releases a latch no key ever consumed so it can't wedge. */
    if (is_modifier_code(code)) {
        if (ev->u.key.down) {
            for (int i = 0; i < inj->n_latched; i++)
                if (inj->latched[i] == code) {  /* already held (Quest re-sends the tap) → keep
                                                 * it latched and refresh the timeout, so Ctrl+C
                                                 * still works when Ctrl arrives as 2 taps. */
                    inj->latch_ms = mono_ms();
                    return;
                }
            if (inj->n_latched < (int)(sizeof inj->latched / sizeof inj->latched[0])) {
                inj->latched[inj->n_latched++] = code;
                inj->latch_ms = mono_ms();
                emit(inj->kbd, EV_KEY, code, 1);
                syn(inj->kbd);
                BSDR_INFO("bsdr.inject", "modifier latched: code=%d (held=%d)",
                          code, inj->n_latched);
            }
        }
        return;                                            /* swallow the up */
    }

    /* The Quest soft keyboard has no PageUp/PageDown keys. Map Ctrl+Up -> PageUp and
     * Ctrl+Down -> PageDown: when an arrow arrives while Ctrl is latched, drop the held Ctrl
     * (so the app sees a clean paging key, not Ctrl+Arrow) and substitute the paging code. Once
     * armed, EVERY further event for that arrow — autorepeat or a re-sent down as well as the
     * key-up — is substituted until the key-up disarms it, so a real (unpaired) arrow press can
     * never leak out and stay stuck. release_remap() + the _handle timeout are the safety nets
     * if the key-up is lost. */
    if (code == KEY_UP || code == KEY_DOWN) {
        if (ev->u.key.down && (ctrl_is_latched(inj) || inj->remap_arrow == code)) {
            if (inj->remap_arrow && inj->remap_arrow != code) release_remap(inj);
            release_all_latched(inj);
            inj->remap_arrow = code;
            inj->remap_ms = mono_ms();
            code = (code == KEY_UP) ? KEY_PAGEUP : KEY_PAGEDOWN;
        } else if (!ev->u.key.down && inj->remap_arrow == code) {
            code = (code == KEY_UP) ? KEY_PAGEUP : KEY_PAGEDOWN;
            inj->remap_arrow = 0;
        }
    }

    if (shift && ev->u.key.down) emit(inj->kbd, EV_KEY, KEY_LEFTSHIFT, 1);
    emit(inj->kbd, EV_KEY, code, ev->u.key.down ? 1 : 0);
    if (shift && !ev->u.key.down) emit(inj->kbd, EV_KEY, KEY_LEFTSHIFT, 0);
    syn(inj->kbd);

    /* Non-modifier key released: drop any latched modifiers now (one-shot). */
    if (!ev->u.key.down) release_all_latched(inj);
}

static void emit_gamepad(struct bsdr_injector *inj, const bsdr_gamepad *g) {
    int fd = inj->pad;
    emit(fd, EV_ABS, ABS_X, g->lx);
    emit(fd, EV_ABS, ABS_Y, g->ly);
    emit(fd, EV_ABS, ABS_RX, g->rx);
    emit(fd, EV_ABS, ABS_RY, g->ry);
    emit(fd, EV_ABS, ABS_Z, g->lt);
    emit(fd, EV_ABS, ABS_RZ, g->rt);
    struct { uint16_t bit; int code; } map[] = {
        { BSDR_XINPUT_A, BTN_A }, { BSDR_XINPUT_B, BTN_B },
        { BSDR_XINPUT_X, BTN_X }, { BSDR_XINPUT_Y, BTN_Y },
        { BSDR_XINPUT_LEFT_SHOULDER, BTN_TL }, { BSDR_XINPUT_RIGHT_SHOULDER, BTN_TR },
        { BSDR_XINPUT_BACK, BTN_SELECT }, { BSDR_XINPUT_START, BTN_START },
        { BSDR_XINPUT_LEFT_THUMB, BTN_THUMBL }, { BSDR_XINPUT_RIGHT_THUMB, BTN_THUMBR },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        emit(fd, EV_KEY, map[i].code, (g->buttons & map[i].bit) ? 1 : 0);
    syn(fd);
}

/* Type-B single-finger touch at inj->touch_x/y. phase: 0 = down, 1 = move, 2 = up. */
static void touch_emit(struct bsdr_injector *inj, int phase) {
    static int track = 0;
    int fd = inj->touch;
    if (fd < 0) return;
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    if (phase == 0) { emit(fd, EV_ABS, ABS_MT_TRACKING_ID, ++track); emit(fd, EV_KEY, BTN_TOUCH, 1); }
    if (phase != 2) {
        emit(fd, EV_ABS, ABS_MT_POSITION_X, inj->touch_x);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, inj->touch_y);
        emit(fd, EV_ABS, ABS_X, inj->touch_x);
        emit(fd, EV_ABS, ABS_Y, inj->touch_y);
    } else { emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1); emit(fd, EV_KEY, BTN_TOUCH, 0); }
    syn(fd);
}

void bsdr_injector_handle(bsdr_injector *inj, const bsdr_input_event *ev) {
    if (!inj->ok) { BSDR_INFO("bsdr.inject", "[null-inject] kind=%d", ev->kind); return; }
    /* Safety net: a sticky modifier that no key consumed within the timeout is released, so a
     * stray Ctrl/Shift tap (or a repeated tap from the Quest) can't wedge the keyboard or turn
     * mouse clicks into Ctrl+click. Checked on every event (mouse moves arrive frequently). */
    if (inj->n_latched > 0 && mono_ms() - inj->latch_ms > BSDR_LATCH_TIMEOUT_MS)
        release_all_latched(inj);
    /* Same net for a paging remap whose arrow key-up never arrived: release the held PageUp/Down
     * so it can't wedge (the crash handler's KEY_ESC..KEY_MICMUTE sweep covers the fatal path). */
    if (inj->remap_arrow && mono_ms() - inj->remap_ms > BSDR_LATCH_TIMEOUT_MS)
        release_remap(inj);
    switch (ev->kind) {
        case BSDR_EV_MOVE_REL:
            emit(inj->mouse, EV_REL, REL_X, ev->u.move_rel.dx);
            emit(inj->mouse, EV_REL, REL_Y, ev->u.move_rel.dy);
            syn(inj->mouse);
            break;
        case BSDR_EV_MOVE_ABS:
            if (g_touch_mode && inj->touch >= 0) {   /* touch: track the point; drag while a finger is down */
                inj->touch_x = (int)(ev->u.move_abs.x * inj->screen_w);
                inj->touch_y = (int)(ev->u.move_abs.y * inj->screen_h);
                if (inj->touching) touch_emit(inj, 1);
                break;
            }
            emit(inj->mouse, EV_ABS, ABS_X, (int)(ev->u.move_abs.x * inj->screen_w));
            emit(inj->mouse, EV_ABS, ABS_Y, (int)(ev->u.move_abs.y * inj->screen_h));
            syn(inj->mouse);
            break;
        case BSDR_EV_BUTTON:
            if (g_touch_mode && inj->touch >= 0 && ev->u.button.button == BSDR_BTN_LEFT) {
                if (ev->u.button.down) { inj->touching = true;  touch_emit(inj, 0); }  /* tap/drag start */
                else                   { inj->touching = false; touch_emit(inj, 2); }  /* lift */
                break;
            }
            if (g_touch_mode && inj->touch >= 0) break;   /* no touch analog for right/middle — ignore */
            emit(inj->mouse, EV_KEY, mouse_btn_code(ev->u.button.button),
                 ev->u.button.down ? 1 : 0);
            syn(inj->mouse);
            break;
        case BSDR_EV_SCROLL:
            if (ev->u.scroll.dy) emit(inj->mouse, EV_REL, REL_WHEEL, ev->u.scroll.dy);
            if (ev->u.scroll.dx) emit(inj->mouse, EV_REL, REL_HWHEEL, ev->u.scroll.dx);
            syn(inj->mouse);
            break;
        case BSDR_EV_KEY:    emit_key(inj, ev); break;
        case BSDR_EV_GAMEPAD: emit_gamepad(inj, &ev->u.gamepad); break;
    }
}

void bsdr_injector_destroy(bsdr_injector *inj) {
    if (!inj) return;
    /* disarm crash cleanup first so a fatal signal mid-teardown won't touch the
     * fds we're about to close */
    g_clean_mouse = g_clean_abs = g_clean_kbd = g_clean_pad = -1;
    release_pressed(inj->mouse, inj->absdev, inj->kbd, inj->pad);  /* no latched buttons */
    destroy_fds(inj->mouse, inj->absdev, inj->kbd, inj->pad);
    if (inj->touch >= 0) { ioctl(inj->touch, UI_DEV_DESTROY); close(inj->touch); }
    free(inj);
}
