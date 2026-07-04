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
/* Minimal HTTP/HTTPS client (OpenSSL for https). Used by the STT and LLM
 * clients and the Bigscreen cloud login. */
#ifndef BSDR_HTTPC_H
#define BSDR_HTTPC_H

#include <stddef.h>

typedef struct { const char *name, *value; } bsdr_http_header;

/* Send one request to `url` (http:// or https://). `body`/`body_len` may be 0.
 * Writes the full raw response (headers+body) into `resp` (cap). Returns the
 * response length, or -1 on failure. Connection: close. */
int bsdr_http_request(const char *method, const char *url,
                      const bsdr_http_header *headers, int nheaders,
                      const char *content_type,
                      const void *body, size_t body_len,
                      char *resp, size_t resp_cap);

/* Parse helpers on a raw response. */
int bsdr_http_status(const char *resp);
const char *bsdr_http_body(const char *resp);   /* points past the headers, or NULL */

#endif /* BSDR_HTTPC_H */
