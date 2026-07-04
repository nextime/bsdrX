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
/* Unit tests for the binary input decoder — mirrors the Python suite. */
#include "bsdr/input_decode.h"
#include "bsdr/protocol.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s\n", msg); failures++; } \
    else printf("PASS %s\n", msg); } while (0)

static void put_i32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }

int main(void) {
    bsdr_input_event ev[4];

    /* relative move */
    uint8_t rel[9]; rel[0] = BSDR_MSG_MOVE_REL; put_i32(rel + 1, 5); put_i32(rel + 5, -3);
    CHECK(bsdr_decode_binary(rel, sizeof(rel), ev, 4) == 1 &&
          ev[0].kind == BSDR_EV_MOVE_REL &&
          ev[0].u.move_rel.dx == 5 && ev[0].u.move_rel.dy == -3,
          "decode_rel_move");

    /* absolute move 0..65535 -> 0..1 */
    uint8_t abs[9]; abs[0] = BSDR_MSG_MOVE_ABS; put_i32(abs + 1, 0); put_i32(abs + 5, 65535);
    CHECK(bsdr_decode_binary(abs, sizeof(abs), ev, 4) == 1 &&
          ev[0].kind == BSDR_EV_MOVE_ABS &&
          ev[0].u.move_abs.x == 0.0 && ev[0].u.move_abs.y == 1.0,
          "decode_abs_move");

    /* buttons incl. X buttons */
    uint8_t b2 = BSDR_MSG_LEFT_DOWN, b5 = BSDR_MSG_RIGHT_UP, b8 = BSDR_MSG_X1_DOWN;
    CHECK(bsdr_decode_binary(&b2, 1, ev, 4) == 1 &&
          ev[0].u.button.button == BSDR_BTN_LEFT && ev[0].u.button.down,
          "decode_left_down");
    CHECK(bsdr_decode_binary(&b5, 1, ev, 4) == 1 &&
          ev[0].u.button.button == BSDR_BTN_RIGHT && !ev[0].u.button.down,
          "decode_right_up");
    CHECK(bsdr_decode_binary(&b8, 1, ev, 4) == 1 &&
          ev[0].u.button.button == BSDR_BTN_X1 && ev[0].u.button.down,
          "decode_x1_down");

    /* wheels */
    uint8_t wv[5]; wv[0] = BSDR_MSG_WHEEL_V; put_i32(wv + 1, 2);
    CHECK(bsdr_decode_binary(wv, sizeof(wv), ev, 4) == 1 &&
          ev[0].kind == BSDR_EV_SCROLL &&
          ev[0].u.scroll.dy == 2 && ev[0].u.scroll.dx == 0,
          "decode_wheel_v");
    uint8_t wh[5]; wh[0] = BSDR_MSG_WHEEL_H; put_i32(wh + 1, -1);
    CHECK(bsdr_decode_binary(wh, sizeof(wh), ev, 4) == 1 &&
          ev[0].u.scroll.dx == -1 && ev[0].u.scroll.dy == 0,
          "decode_wheel_h");

    /* keyboard: raw-VK mode and char mode */
    uint8_t kvk[5]; kvk[0] = BSDR_MSG_KEY; kvk[1] = 1; kvk[2] = 1; put_u16(kvk + 3, 0x41);
    CHECK(bsdr_decode_binary(kvk, sizeof(kvk), ev, 4) == 1 &&
          ev[0].kind == BSDR_EV_KEY && ev[0].u.key.is_vk &&
          ev[0].u.key.value == 0x41 && ev[0].u.key.down,
          "decode_key_vk");
    uint8_t kch[5]; kch[0] = BSDR_MSG_KEY; kch[1] = 0; kch[2] = 0; put_u16(kch + 3, 'a');
    CHECK(bsdr_decode_binary(kch, sizeof(kch), ev, 4) == 1 &&
          !ev[0].u.key.is_vk && ev[0].u.key.value == 'a' && !ev[0].u.key.down,
          "decode_key_char");

    /* gamepad: A pressed, LX full+, RY full-, LT max */
    uint8_t gp[13];
    gp[0] = BSDR_MSG_GAMEPAD;
    put_u16(gp + 1, BSDR_XINPUT_A);
    put_i16(gp + 3, 32767); put_i16(gp + 5, 0);
    put_i16(gp + 7, 0); put_i16(gp + 9, -32768);
    gp[11] = 255; gp[12] = 0;
    CHECK(bsdr_decode_binary(gp, sizeof(gp), ev, 4) == 1 &&
          ev[0].kind == BSDR_EV_GAMEPAD &&
          (ev[0].u.gamepad.buttons & BSDR_XINPUT_A) &&
          ev[0].u.gamepad.lx == 32767 && ev[0].u.gamepad.ry == -32768 &&
          ev[0].u.gamepad.lt == 255 && ev[0].u.gamepad.rt == 0,
          "decode_gamepad");

    /* bad frames */
    uint8_t bad = 0xFF;
    uint8_t shortrel[2] = { BSDR_MSG_MOVE_REL, 0x02 };
    CHECK(bsdr_decode_binary(NULL, 0, ev, 4) == 0, "decode_empty");
    CHECK(bsdr_decode_binary(shortrel, 2, ev, 4) == 0, "decode_short");
    CHECK(bsdr_decode_binary(&bad, 1, ev, 4) == 0, "decode_unknown");

    printf(failures ? "\nFAILED (%d)\n" : "\nOK - all input_decode tests passed\n",
           failures);
    return failures ? 1 : 0;
}
