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
/* Logging-only injector — used on unknown platforms and as the fallback when a
 * real backend can't open its device. */
#include "bsdr/inject.h"
#include "bsdr/log.h"

#include <stdlib.h>

struct bsdr_injector { int dummy; };

bsdr_injector *bsdr_injector_create(int screen_w, int screen_h) {
    (void)screen_w; (void)screen_h;
    BSDR_WARN("bsdr.inject", "no native injector for this platform; logging only");
    return calloc(1, sizeof(struct bsdr_injector));
}

void bsdr_injector_handle(bsdr_injector *inj, const bsdr_input_event *ev) {
    (void)inj;
    BSDR_INFO("bsdr.inject", "[null-inject] event kind=%d", ev->kind);
}

void bsdr_injector_destroy(bsdr_injector *inj) { free(inj); }
