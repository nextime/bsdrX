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
/* macOS input injection via CoreGraphics CGEvent (mouse + keyboard + scroll).
 * No standard virtual-gamepad API on macOS, so gamepad events are logged.
 * Falls back to logging if the event source can't be created. */
#include "bsdr/inject.h"
#include "bsdr/log.h"

#if defined(__APPLE__)

#include <ApplicationServices/ApplicationServices.h>
#include <stdlib.h>
#include <stdbool.h>

/* macOS virtual keycodes (Carbon kVK_*) for the nav keys we can drive. The Quest sends Windows
 * VKs; map the ones we act on. Other vk keys keep the existing (layout-0) path unchanged. */
enum { MVK_UP = 126, MVK_DOWN = 125, MVK_PAGEUP = 116, MVK_PAGEDOWN = 121 };

/* Windows VK -> macOS CGKeyCode (Carbon kVK_*) for the keys the Quest sends as virtual keys, plus
 * letters/digits so latched Ctrl can form real Ctrl+<key> combos. Returns -1 for anything unmapped
 * so the caller SKIPS it — the old code fell through to keycode 0, which is 'A', so every unmapped
 * navigation key silently typed the letter 'a'. */
static int vk_to_cgkey(uint16_t vk) {
    switch (vk) {
        case 0x08: return 51;  case 0x09: return 48;  case 0x0D: return 36;  /* Bksp Tab Return */
        case 0x1B: return 53;  case 0x20: return 49;  case 0x2E: return 117; /* Esc Space FwdDel */
        case 0x25: return 123; case 0x27: return 124;                        /* Left Right       */
        case 0x26: return MVK_UP;   case 0x28: return MVK_DOWN;
        case 0x24: return 115; case 0x23: return 119;                        /* Home End         */
        case 0x21: return MVK_PAGEUP; case 0x22: return MVK_PAGEDOWN;
        case 'A': return 0;  case 'B': return 11; case 'C': return 8;  case 'D': return 2;
        case 'E': return 14; case 'F': return 3;  case 'G': return 5;  case 'H': return 4;
        case 'I': return 34; case 'J': return 38; case 'K': return 40; case 'L': return 37;
        case 'M': return 46; case 'N': return 45; case 'O': return 31; case 'P': return 35;
        case 'Q': return 12; case 'R': return 15; case 'S': return 1;  case 'T': return 17;
        case 'U': return 32; case 'V': return 9;  case 'W': return 13; case 'X': return 7;
        case 'Y': return 16; case 'Z': return 6;
        case '0': return 29; case '1': return 18; case '2': return 19; case '3': return 20;
        case '4': return 21; case '5': return 23; case '6': return 22; case '7': return 26;
        case '8': return 28; case '9': return 25;
        default: return -1;
    }
}

struct bsdr_injector {
    CGEventSourceRef src;
    CGFloat w, h;
    CGPoint pos;          /* tracked cursor for relative moves */
    /* Ctrl+Up/Down -> PageUp/PageDown: the Quest taps Ctrl (no PageUp/PageDown keys). Latch the
     * Ctrl tap (swallowed) and remap the following arrow; remap_arrow pairs down with its up. */
    bool ctrl_held;
    uint16_t remap_arrow;   /* Windows VK_UP(0x26)/VK_DOWN(0x28) remapped (0 = none) */
};

bsdr_injector *bsdr_injector_create(int screen_w, int screen_h) {
    struct bsdr_injector *inj = calloc(1, sizeof(*inj));
    if (!inj) return NULL;
    inj->src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGDirectDisplayID disp = CGMainDisplayID();
    inj->w = screen_w > 0 ? screen_w : (CGFloat)CGDisplayPixelsWide(disp);
    inj->h = screen_h > 0 ? screen_h : (CGFloat)CGDisplayPixelsHigh(disp);
    inj->pos = CGPointMake(inj->w / 2, inj->h / 2);
    if (!inj->src)
        BSDR_WARN("bsdr.inject", "CGEventSource unavailable; logging only");
    else
        BSDR_INFO("bsdr.inject", "macOS CGEvent injector ready "
                  "(gamepad not supported)");
    return inj;
}

static void post_mouse(struct bsdr_injector *inj, CGEventType type,
                       CGMouseButton btn) {
    CGEventRef e = CGEventCreateMouseEvent(inj->src, type, inj->pos, btn);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

void bsdr_injector_handle(bsdr_injector *inj, const bsdr_input_event *ev) {
    if (!inj->src) { BSDR_INFO("bsdr.inject", "[null-inject] kind=%d", ev->kind); return; }
    switch (ev->kind) {
        case BSDR_EV_MOVE_REL:
            inj->pos.x += ev->u.move_rel.dx;
            inj->pos.y += ev->u.move_rel.dy;
            post_mouse(inj, kCGEventMouseMoved, kCGMouseButtonLeft);
            break;
        case BSDR_EV_MOVE_ABS:
            inj->pos.x = ev->u.move_abs.x * inj->w;
            inj->pos.y = ev->u.move_abs.y * inj->h;
            post_mouse(inj, kCGEventMouseMoved, kCGMouseButtonLeft);
            break;
        case BSDR_EV_BUTTON: {
            bool d = ev->u.button.down;
            CGMouseButton b = kCGMouseButtonLeft;
            CGEventType t = kCGEventLeftMouseDown;
            switch (ev->u.button.button) {
                case BSDR_BTN_LEFT:  b = kCGMouseButtonLeft;
                    t = d ? kCGEventLeftMouseDown : kCGEventLeftMouseUp; break;
                case BSDR_BTN_RIGHT: b = kCGMouseButtonRight;
                    t = d ? kCGEventRightMouseDown : kCGEventRightMouseUp; break;
                default:             b = kCGMouseButtonCenter;
                    t = d ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; break;
            }
            post_mouse(inj, t, b);
            break;
        }
        case BSDR_EV_SCROLL: {
            CGEventRef e = CGEventCreateScrollWheelEvent(
                inj->src, kCGScrollEventUnitLine, 2,
                ev->u.scroll.dy, ev->u.scroll.dx);
            CGEventPost(kCGHIDEventTap, e);
            CFRelease(e);
            break;
        }
        case BSDR_EV_KEY: {
            bool down = ev->u.key.down;
            /* Non-VK: plain text. If Ctrl is latched and this is an ASCII letter/digit, emit it as a
             * keycode with the Control flag so Ctrl+C/V/etc. reach the app (the old code swallowed
             * the Ctrl tap and only typed the letter). Otherwise post the Unicode string as before. */
            if (!ev->u.key.is_vk) {
                uint16_t ch = ev->u.key.value;
                int kc = -1;
                if (inj->ctrl_held && ch < 0x80) {
                    int up = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
                    kc = vk_to_cgkey((uint16_t)up);
                }
                if (kc >= 0) {
                    CGEventRef e = CGEventCreateKeyboardEvent(inj->src, (CGKeyCode)kc, down);
                    CGEventSetFlags(e, kCGEventFlagMaskControl);
                    CGEventPost(kCGHIDEventTap, e);
                    CFRelease(e);
                } else {
                    CGEventRef e = CGEventCreateKeyboardEvent(inj->src, 0, down);
                    UniChar u = (UniChar)ch;
                    CGEventKeyboardSetUnicodeString(e, 1, &u);
                    CGEventPost(kCGHIDEventTap, e);
                    CFRelease(e);
                }
                if (!down) inj->ctrl_held = false;
                break;
            }
            uint16_t vk = ev->u.key.value;
            if (vk == 0x11 || vk == 0xA2 || vk == 0xA3) { inj->ctrl_held = true; break; } /* latch Ctrl */
            int kc;
            if ((vk == 0x26 || vk == 0x28) && down && inj->ctrl_held) {
                inj->remap_arrow = vk;
                kc = (vk == 0x26) ? MVK_PAGEUP : MVK_PAGEDOWN;
                inj->ctrl_held = false;                 /* consumed as paging, not Ctrl+Arrow */
            } else if ((vk == 0x26 || vk == 0x28) && !down && inj->remap_arrow == vk) {
                kc = (vk == 0x26) ? MVK_PAGEUP : MVK_PAGEDOWN;
                inj->remap_arrow = 0;
            } else {
                kc = vk_to_cgkey(vk);
                if (kc < 0) {   /* unmapped: skip rather than injecting keycode 0 ('a') */
                    BSDR_DEBUG("bsdr.inject", "unmapped vk 0x%02x", vk);
                    inj->ctrl_held = false;
                    break;
                }
            }
            CGEventRef e = CGEventCreateKeyboardEvent(inj->src, (CGKeyCode)kc, down);
            if (inj->ctrl_held) CGEventSetFlags(e, kCGEventFlagMaskControl);
            CGEventPost(kCGHIDEventTap, e);
            CFRelease(e);
            if (!down) inj->ctrl_held = false;
            break;
        }
        case BSDR_EV_GAMEPAD:
            BSDR_DEBUG("bsdr.inject", "gamepad event (unsupported on macOS)");
            break;
    }
}

void bsdr_injector_destroy(bsdr_injector *inj) {
    if (!inj) return;
    if (inj->src) CFRelease(inj->src);
    free(inj);
}

#endif /* __APPLE__ */

/* macOS has no public touch-injection API — touch mode falls back to the mouse path above. */
void bsdr_injector_touch_mode(int on) { (void)on; }
