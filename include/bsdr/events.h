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
/* Transport-neutral input event model.
 *
 * The DataChannel binary format is decoded into these tagged events; the
 * platform injector (uinput / SendInput / CGEvent) maps them to native input.
 * Values are kept close to the wire so the mapping lives in one place per OS:
 *   - abs move coords are normalized 0..1
 *   - rel move / wheel deltas are signed ints
 *   - keys carry a 16-bit value + `is_vk` (Windows virtual-key vs ASCII char)
 *   - gamepad carries the raw XInput layout (wButtons mask + axes)
 */
#ifndef BSDR_EVENTS_H
#define BSDR_EVENTS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BSDR_EV_MOVE_REL = 1,
    BSDR_EV_MOVE_ABS,
    BSDR_EV_BUTTON,
    BSDR_EV_SCROLL,
    BSDR_EV_KEY,
    BSDR_EV_GAMEPAD
} bsdr_event_kind;

typedef enum {
    BSDR_BTN_LEFT = 0,
    BSDR_BTN_RIGHT,
    BSDR_BTN_MIDDLE,
    BSDR_BTN_X1,
    BSDR_BTN_X2
} bsdr_mouse_button;

typedef struct {
    uint16_t buttons;   /* XInput wButtons bitmask */
    int16_t lx, ly, rx, ry;  /* sticks, -32768..32767 */
    uint8_t lt, rt;     /* triggers, 0..255 */
} bsdr_gamepad;

typedef struct {
    bsdr_event_kind kind;
    union {
        struct { int32_t dx, dy; } move_rel;
        struct { double x, y; } move_abs;          /* 0..1 */
        struct { bsdr_mouse_button button; bool down; } button;
        struct { int32_t dx, dy; } scroll;          /* notches */
        struct { uint16_t value; bool is_vk; bool down; } key;
        bsdr_gamepad gamepad;
    } u;
} bsdr_input_event;

/* XInput wButtons bits (for injector mapping). */
#define BSDR_XINPUT_DPAD_UP        0x0001
#define BSDR_XINPUT_DPAD_DOWN      0x0002
#define BSDR_XINPUT_DPAD_LEFT      0x0004
#define BSDR_XINPUT_DPAD_RIGHT     0x0008
#define BSDR_XINPUT_START          0x0010
#define BSDR_XINPUT_BACK           0x0020
#define BSDR_XINPUT_LEFT_THUMB     0x0040
#define BSDR_XINPUT_RIGHT_THUMB    0x0080
#define BSDR_XINPUT_LEFT_SHOULDER  0x0100
#define BSDR_XINPUT_RIGHT_SHOULDER 0x0200
#define BSDR_XINPUT_A              0x1000
#define BSDR_XINPUT_B              0x2000
#define BSDR_XINPUT_X              0x4000
#define BSDR_XINPUT_Y              0x8000

#endif /* BSDR_EVENTS_H */
