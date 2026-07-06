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
/* Android input injection. Non-root means no /dev/uinput: events are marshalled
 * to the Kotlin AccessibilityService (bsdr_android_emit_input), which dispatches
 * gestures (tap/drag/scroll) and text. This is the documented non-root ceiling —
 * coarse touch + text, not a pixel-precise pointer; right/middle/X buttons and
 * gamepad have no Accessibility analog and are dropped (a root build would map
 * them to uinput behind this same switch). Mirrors inject_null.c's "never NULL,
 * degrade to logging" contract. */
#include "bsdr/inject.h"
#include "bsdr/log.h"
#include "bsdr_android.h"

#include <stdlib.h>

/* Android KeyEvent keycodes for the nav keys we can drive via performGlobalAction
 * / dispatched key events (the AccessibilityService maps these). */
enum {
    AKEY_BACK = 4, AKEY_DPAD_UP = 19, AKEY_DPAD_DOWN = 20, AKEY_DPAD_LEFT = 21,
    AKEY_DPAD_RIGHT = 22, AKEY_HOME = 3, AKEY_ENTER = 66, AKEY_DEL = 67,
    AKEY_TAB = 61, AKEY_ESCAPE = 111
};

struct bsdr_injector {
    int w, h;          /* screen size for abs-pointer mapping */
    int cx, cy;        /* current cursor position (px) */
    bool down;         /* a left-button gesture is in progress */
    bool ctrl_held;    /* Quest tapped Ctrl; next Up/Down becomes a page scroll (no PageUp key) */
};

bsdr_injector *bsdr_injector_create(int screen_w, int screen_h) {
    bsdr_injector *inj = calloc(1, sizeof(*inj));
    if (!inj) return NULL;
    inj->w = screen_w > 0 ? screen_w : 1080;
    inj->h = screen_h > 0 ? screen_h : 1920;
    inj->cx = inj->w / 2; inj->cy = inj->h / 2;
    BSDR_INFO("bsdr.inject", "android injector -> AccessibilityService (%dx%d, gestures+text)",
              inj->w, inj->h);
    return inj;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Map a Windows virtual-key to an Android keycode for the nav keys we support. */
static int vk_to_akey(uint16_t vk) {
    switch (vk) {
        case 0x0D: return AKEY_ENTER;       case 0x08: return AKEY_DEL;
        case 0x09: return AKEY_TAB;         case 0x1B: return AKEY_ESCAPE;
        case 0x25: return AKEY_DPAD_LEFT;   case 0x26: return AKEY_DPAD_UP;
        case 0x27: return AKEY_DPAD_RIGHT;  case 0x28: return AKEY_DPAD_DOWN;
        case 0x24: return AKEY_HOME;
        default:   return -1;
    }
}

void bsdr_injector_handle(bsdr_injector *inj, const bsdr_input_event *ev) {
    if (!inj) return;
    switch (ev->kind) {
        case BSDR_EV_MOVE_ABS:
            inj->cx = clampi((int)(ev->u.move_abs.x * inj->w), 0, inj->w - 1);
            inj->cy = clampi((int)(ev->u.move_abs.y * inj->h), 0, inj->h - 1);
            bsdr_android_emit_input(BSDR_AINPUT_MOVE, inj->cx, inj->cy);
            break;
        case BSDR_EV_MOVE_REL:
            inj->cx = clampi(inj->cx + ev->u.move_rel.dx, 0, inj->w - 1);
            inj->cy = clampi(inj->cy + ev->u.move_rel.dy, 0, inj->h - 1);
            bsdr_android_emit_input(BSDR_AINPUT_MOVE, inj->cx, inj->cy);
            break;
        case BSDR_EV_BUTTON:
            if (ev->u.button.button != BSDR_BTN_LEFT) {  /* no Accessibility analog */
                BSDR_DEBUG("bsdr.inject", "drop non-left button %d (non-root limit)",
                           ev->u.button.button);
                break;
            }
            if (ev->u.button.down) {
                inj->down = true;
                bsdr_android_emit_input(BSDR_AINPUT_DOWN, inj->cx, inj->cy);
            } else {
                inj->down = false;
                bsdr_android_emit_input(BSDR_AINPUT_UP, inj->cx, inj->cy);
            }
            break;
        case BSDR_EV_SCROLL:
            bsdr_android_emit_input(BSDR_AINPUT_SCROLL, ev->u.scroll.dx, ev->u.scroll.dy);
            break;
        case BSDR_EV_KEY:
            /* Ctrl is tapped by the Quest before the next key; latch it (down or up) and swallow. */
            if (ev->u.key.is_vk &&
                (ev->u.key.value == 0x11 || ev->u.key.value == 0xA2 || ev->u.key.value == 0xA3)) {
                inj->ctrl_held = true;
                break;
            }
            if (!ev->u.key.down) break;                  /* fire on press only */
            if (ev->u.key.is_vk) {
                /* Android has no PageUp/PageDown key; the closest analog is a page-sized scroll
                 * gesture. Ctrl+Up -> scroll up, Ctrl+Down -> scroll down (sign may need tuning). */
                if (inj->ctrl_held && (ev->u.key.value == 0x26 || ev->u.key.value == 0x28)) {
                    int dy = (ev->u.key.value == 0x26) ? -inj->h * 3 / 4 : inj->h * 3 / 4;
                    bsdr_android_emit_input(BSDR_AINPUT_SCROLL, 0, dy);
                    inj->ctrl_held = false;
                    break;
                }
                inj->ctrl_held = false;
                int ak = vk_to_akey(ev->u.key.value);
                if (ak >= 0) bsdr_android_emit_input(BSDR_AINPUT_KEY, ak, 0);
                else BSDR_DEBUG("bsdr.inject", "unmapped vk 0x%02x", ev->u.key.value);
            } else {
                inj->ctrl_held = false;
                bsdr_android_emit_input(BSDR_AINPUT_TEXT, ev->u.key.value, 0);  /* unicode */
            }
            break;
        case BSDR_EV_GAMEPAD:                             /* no Accessibility analog */
            BSDR_DEBUG("bsdr.inject", "drop gamepad event (non-root limit)");
            break;
    }
}

void bsdr_injector_destroy(bsdr_injector *inj) { free(inj); }

/* Android injects via the AccessibilityService (Kotlin) — gestures already; pointer mode is n/a here. */
void bsdr_injector_touch_mode(int on) { (void)on; }
