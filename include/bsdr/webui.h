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

/* Start the control panel. bind_addr = the listen address (NULL/"" => "127.0.0.1", loopback only;
 * "0.0.0.0" = all interfaces; or a specific IP). allow_hosts = comma-separated extra Host/Origin
 * values the CSRF guard should accept beyond loopback (a LAN IP, or the nginx server_name when behind
 * a reverse proxy); "*" accepts any Host/Origin (trust the proxy / your network). NULL/"" => loopback
 * only, unchanged. Binding off-loopback exposes an UNAUTHENTICATED panel — put it behind a proxy that
 * adds auth, or a firewall. */
bsdr_webui *bsdr_webui_start(bsdr_app *app, uint16_t port, const char *bind_addr, const char *allow_hosts);
void bsdr_webui_stop(bsdr_webui *w);

/* HTTP reply helper handed to loadable plugins (see bsdr/plugin.h). `conn` is the opaque handle the
 * plugin received in its http() hook; it points at the live request socket. Not for general use. */
#include <stddef.h>
void bsdr_webui_plugin_respond(void *conn, int code, const char *ctype, const char *body, size_t len);

#endif /* BSDR_WEBUI_H */
