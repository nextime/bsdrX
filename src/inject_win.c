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
/* Windows input injection via SendInput (mouse + keyboard) — the same Win32 API
 * BigSoup.dll itself uses. Gamepad needs ViGEmBus (optional, not bundled): we
 * log gamepad events with a note. Falls back to logging if SendInput fails. */
#include "bsdr/inject.h"
#include "bsdr/log.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <stdbool.h>

/* The Quest soft keyboard sends Ctrl as a momentary tap (down+up) before the next key, and has
 * no PageUp/PageDown keys. We latch a Ctrl tap, then remap Ctrl+Up -> PageUp / Ctrl+Down ->
 * PageDown (VK_PRIOR/VK_NEXT). The Ctrl tap itself is swallowed — these platforms never combined
 * a tapped modifier with the following key anyway, so nothing regresses. remap_arrow pairs the
 * key-up with the same substitution so down/up stay matched. */
struct bsdr_injector {
    bool ctrl_held;
    WORD remap_arrow;   /* VK_UP/VK_DOWN whose down we remapped (0 = none) */
    int  sw, sh;        /* screen pixels, for touch injection coordinates */
    int  tx, ty;        /* last pointer pixel position (for a touch-down / drag) */
    bool touching;
};

static bool is_ctrl_vk(WORD vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }

/* Pointer mode (process-global; the injector is per-session). 0 = mouse, 1 = real touch. */
static int g_touch_mode = 0;
void bsdr_injector_touch_mode(int on) { g_touch_mode = on ? 1 : 0; }

bsdr_injector *bsdr_injector_create(int screen_w, int screen_h) {
    BSDR_INFO("bsdr.inject", "Windows SendInput injector ready "
              "(gamepad requires ViGEmBus; not bundled)");
    struct bsdr_injector *inj = calloc(1, sizeof(struct bsdr_injector));
    if (inj) {
        inj->sw = screen_w > 0 ? screen_w : GetSystemMetrics(SM_CXSCREEN);
        inj->sh = screen_h > 0 ? screen_h : GetSystemMetrics(SM_CYSCREEN);
    }
    return inj;
}

/* Real touch injection (Windows 8+). phase: 0 down, 1 move, 2 up. One finger at (px,py) pixels. */
static void touch_win(int phase, int px, int py) {
    static int ready = -1;
    if (ready < 0) ready = InitializeTouchInjection(1, TOUCH_FEEDBACK_DEFAULT) ? 1 : 0;
    if (ready != 1) return;
    POINTER_TOUCH_INFO c;
    ZeroMemory(&c, sizeof c);
    c.pointerInfo.pointerType = PT_TOUCH;
    c.pointerInfo.pointerId = 0;
    c.pointerInfo.ptPixelLocation.x = px;
    c.pointerInfo.ptPixelLocation.y = py;
    c.touchFlags = TOUCH_FLAG_NONE;
    c.touchMask  = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
    c.pressure   = 32000;
    c.rcContact.left = px - 2; c.rcContact.right = px + 2;
    c.rcContact.top  = py - 2; c.rcContact.bottom = py + 2;
    c.pointerInfo.pointerFlags = (phase == 0) ? (POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)
                               : (phase == 1) ? (POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)
                                              : POINTER_FLAG_UP;
    InjectTouchInput(1, &c);
}

static void send_mouse(DWORD flags, LONG dx, LONG dy, DWORD data) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.mouseData = data;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(INPUT));
}

static void send_key_vk(WORD vk, BOOL down) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

static void send_key_unicode(WCHAR ch, BOOL down) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

void bsdr_injector_handle(bsdr_injector *inj, const bsdr_input_event *ev) {
    switch (ev->kind) {
        case BSDR_EV_MOVE_REL:
            send_mouse(MOUSEEVENTF_MOVE, ev->u.move_rel.dx, ev->u.move_rel.dy, 0);
            break;
        case BSDR_EV_MOVE_ABS:
            if (g_touch_mode && inj) {            /* touch: track the point; drag while a finger is down */
                inj->tx = (int)(ev->u.move_abs.x * inj->sw);
                inj->ty = (int)(ev->u.move_abs.y * inj->sh);
                if (inj->touching) touch_win(1, inj->tx, inj->ty);
                break;
            }
            send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                       (LONG)(ev->u.move_abs.x * 65535.0),
                       (LONG)(ev->u.move_abs.y * 65535.0), 0);
            break;
        case BSDR_EV_BUTTON: {
            BOOL d = ev->u.button.down;
            if (g_touch_mode && inj) {
                if (ev->u.button.button == BSDR_BTN_LEFT) {
                    if (d) { inj->touching = true;  touch_win(0, inj->tx, inj->ty); }
                    else   { inj->touching = false; touch_win(2, inj->tx, inj->ty); }
                }
                break;                            /* right/middle have no touch analog — ignore */
            }
            switch (ev->u.button.button) {
                case BSDR_BTN_LEFT:
                    send_mouse(d ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0); break;
                case BSDR_BTN_RIGHT:
                    send_mouse(d ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0, 0, 0); break;
                case BSDR_BTN_MIDDLE:
                    send_mouse(d ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP, 0, 0, 0); break;
                case BSDR_BTN_X1:
                    send_mouse(d ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON1); break;
                case BSDR_BTN_X2:
                    send_mouse(d ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON2); break;
            }
            break;
        }
        case BSDR_EV_SCROLL:
            if (ev->u.scroll.dy)
                send_mouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(ev->u.scroll.dy * WHEEL_DELTA));
            if (ev->u.scroll.dx)
                send_mouse(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)(ev->u.scroll.dx * WHEEL_DELTA));
            break;
        case BSDR_EV_KEY: {
            BOOL down = ev->u.key.down;
            WORD vk;
            if (!ev->u.key.is_vk) {
                /* Plain text. If Ctrl is latched and this is an ASCII letter/digit, re-express it as
                 * a VK so it can be wrapped in Ctrl below — KEYEVENTF_UNICODE ignores modifiers, so
                 * the old path turned Ctrl+C into a literal "c". Otherwise inject the Unicode char. */
                uint16_t ch = ev->u.key.value;
                int letter = ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z');
                int digit  = (ch >= '0' && ch <= '9');
                if (inj->ctrl_held && ch < 0x80 && (letter || digit)) {
                    vk = letter ? (WORD)(ch & ~0x20) : (WORD)ch;   /* VK_A..Z / VK_0..9 */
                } else {
                    send_key_unicode((WCHAR)ch, down);
                    if (!down) inj->ctrl_held = false;
                    break;
                }
            } else {
                vk = (WORD)ev->u.key.value;
                if (is_ctrl_vk(vk)) { inj->ctrl_held = true; break; }   /* latch + swallow the tap */
                if ((vk == VK_UP || vk == VK_DOWN) && down && inj->ctrl_held) {
                    inj->remap_arrow = vk;
                    vk = (vk == VK_UP) ? VK_PRIOR : VK_NEXT;            /* PageUp / PageDown */
                    inj->ctrl_held = false;                            /* paging, not Ctrl+Arrow */
                } else if ((vk == VK_UP || vk == VK_DOWN) && !down && inj->remap_arrow == vk) {
                    vk = (vk == VK_UP) ? VK_PRIOR : VK_NEXT;
                    inj->remap_arrow = 0;
                }
            }
            /* Emit, wrapping in a real Ctrl press if one is latched so Ctrl+<key> combos work. */
            if (inj->ctrl_held) {
                if (down) send_key_vk(VK_CONTROL, TRUE);
                send_key_vk(vk, down);
                if (!down) { send_key_vk(VK_CONTROL, FALSE); inj->ctrl_held = false; }
            } else {
                send_key_vk(vk, down);
            }
            break;
        }
        case BSDR_EV_GAMEPAD:
            BSDR_DEBUG("bsdr.inject", "gamepad event (needs ViGEmBus; not injected)");
            break;
    }
}

void bsdr_injector_destroy(bsdr_injector *inj) { free(inj); }

#endif /* _WIN32 */
