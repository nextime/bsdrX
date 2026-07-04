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
#include "bsdr/input_decode.h"
#include "bsdr/protocol.h"
#include "bsdr/log.h"

static int32_t rd_i32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static int16_t rd_i16(const uint8_t *p) { return (int16_t)rd_u16(p); }

static size_t button(bsdr_input_event *e, bsdr_mouse_button b, bool down) {
    e->kind = BSDR_EV_BUTTON;
    e->u.button.button = b;
    e->u.button.down = down;
    return 1;
}

size_t bsdr_decode_binary(const uint8_t *data, size_t len,
                          bsdr_input_event *out, size_t max) {
    if (len < 1 || max < 1) return 0;
    bsdr_input_event *e = &out[0];

    switch (data[0]) {
        case BSDR_MSG_MOVE_ABS:
            if (len < 9) break;
            e->kind = BSDR_EV_MOVE_ABS;
            e->u.move_abs.x = (double)rd_i32(data + 1) / 65535.0;
            e->u.move_abs.y = (double)rd_i32(data + 5) / 65535.0;
            return 1;
        case BSDR_MSG_MOVE_REL:
            if (len < 9) break;
            e->kind = BSDR_EV_MOVE_REL;
            e->u.move_rel.dx = rd_i32(data + 1);
            e->u.move_rel.dy = rd_i32(data + 5);
            return 1;
        case BSDR_MSG_LEFT_DOWN:   return button(e, BSDR_BTN_LEFT, true);
        case BSDR_MSG_LEFT_UP:     return button(e, BSDR_BTN_LEFT, false);
        case BSDR_MSG_RIGHT_DOWN:  return button(e, BSDR_BTN_RIGHT, true);
        case BSDR_MSG_RIGHT_UP:    return button(e, BSDR_BTN_RIGHT, false);
        case BSDR_MSG_MIDDLE_DOWN: return button(e, BSDR_BTN_MIDDLE, true);
        case BSDR_MSG_MIDDLE_UP:   return button(e, BSDR_BTN_MIDDLE, false);
        case BSDR_MSG_X1_DOWN:     return button(e, BSDR_BTN_X1, true);
        case BSDR_MSG_X1_UP:       return button(e, BSDR_BTN_X1, false);
        case BSDR_MSG_X2_DOWN:     return button(e, BSDR_BTN_X2, true);
        case BSDR_MSG_X2_UP:       return button(e, BSDR_BTN_X2, false);
        case BSDR_MSG_WHEEL_V:
            if (len < 5) break;
            e->kind = BSDR_EV_SCROLL;
            e->u.scroll.dx = 0;
            e->u.scroll.dy = rd_i32(data + 1);   /* notches; Win scales *120 */
            return 1;
        case BSDR_MSG_WHEEL_H:
            if (len < 5) break;
            e->kind = BSDR_EV_SCROLL;
            e->u.scroll.dx = rd_i32(data + 1);
            e->u.scroll.dy = 0;
            return 1;
        case BSDR_MSG_KEY:
            /* byte[1]=down(1)/up(0)  byte[2]=mode(0 char / !=0 raw VK)
             * u16le[3:5]=value (low byte=ASCII char in char mode, else VK). */
            if (len < 5) break;
            e->kind = BSDR_EV_KEY;
            e->u.key.down = data[1] != 0;
            e->u.key.is_vk = data[2] != 0;
            e->u.key.value = rd_u16(data + 3);
            return 1;
        case BSDR_MSG_GAMEPAD:
            /* XINPUT_GAMEPAD: u16 buttons | i16 LX,LY,RX,RY | u8 LT,RT */
            if (len < 13) break;
            e->kind = BSDR_EV_GAMEPAD;
            e->u.gamepad.buttons = rd_u16(data + 1);
            e->u.gamepad.lx = rd_i16(data + 3);
            e->u.gamepad.ly = rd_i16(data + 5);
            e->u.gamepad.rx = rd_i16(data + 7);
            e->u.gamepad.ry = rd_i16(data + 9);
            e->u.gamepad.lt = data[11];
            e->u.gamepad.rt = data[12];
            return 1;
        default:
            BSDR_INFO("bsdr.decode", "unknown input type 0x%02x (%zu bytes)",
                      data[0], len);
            return 0;
    }
    BSDR_WARN("bsdr.decode", "short 0x%02x frame (%zu bytes)", data[0], len);
    return 0;
}
