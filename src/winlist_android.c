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
/* Android window enumeration: there is no per-window capture under
 * MediaProjection (it casts the whole display), so the web UI's window picker
 * is empty and the agent always streams the full screen. */
#include "bsdr/winlist.h"

int bsdr_list_windows(const char *display, bsdr_window *out, int max) {
    (void)display; (void)out; (void)max;
    return 0;
}

/* No monitor enumeration either: MediaProjection exposes a single logical display
 * whose geometry is only known at capture time, so the picker stays empty and the
 * agent streams the whole screen. Mirrors bsdr_list_windows above. */
int bsdr_list_monitors(const char *display, bsdr_window *out, int max) {
    (void)display; (void)out; (void)max;
    return 0;
}
