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
/* Local control web UI: serves a page + JSON API on 127.0.0.1 for Bigscreen
 * login, connection status, Quest selection, source, and stop/restart. */
#ifndef BSDR_WEBUI_H
#define BSDR_WEBUI_H

#include "bsdr/app.h"
#include <stdint.h>

typedef struct bsdr_webui bsdr_webui;

bsdr_webui *bsdr_webui_start(bsdr_app *app, uint16_t port);
void bsdr_webui_stop(bsdr_webui *w);

#endif /* BSDR_WEBUI_H */
