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
/* Decode headset->PC DataChannel input messages into bsdr_input_event[].
 *
 * Wire format recovered by static disassembly of BigSoup.dll (see the spec).
 * One binary DataChannel message == one input event; byte[0] = type; integers
 * are little-endian.
 */
#ifndef BSDR_INPUT_DECODE_H
#define BSDR_INPUT_DECODE_H

#include "bsdr/events.h"
#include <stddef.h>
#include <stdint.h>

/* Decode one message into `out` (capacity `max`). Returns the number of events
 * produced (0 on a short/unknown frame, otherwise 1). Key events are neutral
 * (value + is_vk); the platform injector maps to native keycodes incl. Shift. */
size_t bsdr_decode_binary(const uint8_t *data, size_t len,
                          bsdr_input_event *out, size_t max);

#endif /* BSDR_INPUT_DECODE_H */
