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
/* Cross-platform input injection — the analog of the agent's SendInput +
 * ViGEmBus. One backend is compiled per OS (uinput / SendInput / CGEvent); each
 * falls back to logging if the real device can't be opened. */
#ifndef BSDR_INJECT_H
#define BSDR_INJECT_H

#include "bsdr/events.h"

typedef struct bsdr_injector bsdr_injector;

/* Create the platform injector. Never returns NULL: on failure it yields a
 * logging stub so the rest of the agent still runs. `screen_w/h` size the
 * absolute-pointer mapping (ignored where the OS provides screen bounds). */
bsdr_injector *bsdr_injector_create(int screen_w, int screen_h);

void bsdr_injector_handle(bsdr_injector *inj, const bsdr_input_event *ev);
void bsdr_injector_destroy(bsdr_injector *inj);

/* Pointer mode (process-global; the injector is per-session so this is the live toggle the web UI
 * flips): 0 = mouse (absolute move + click; a tap is a click, hold+move is a drag), 1 = TOUCH — map
 * the headset's pointer to real touchscreen events (tap/drag) where the OS supports injecting them
 * (Linux uinput multitouch, Windows InjectTouchInput). macOS has no public touch-injection API, so it
 * stays in mouse mode regardless. Safe to call any time, including before any injector exists. */
void bsdr_injector_touch_mode(int on);

#endif /* BSDR_INJECT_H */
