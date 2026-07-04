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
/* Minimal JSON helpers — just enough for Bigscreen's small flat control bodies.
 * Not a general parser: scans for top-level "key": value pairs. */
#ifndef BSDR_JSON_H
#define BSDR_JSON_H

#include <stdbool.h>
#include <stddef.h>

/* Extract a string field: "key":"value". Unescapes \" \\ \/ \n \t \r. */
bool bsdr_json_get_str(const char *json, const char *key,
                       char *out, size_t outlen);

/* Extract a numeric field: "key": 123(.45). */
bool bsdr_json_get_double(const char *json, const char *key, double *out);

/* Append "key":"value" (escaped) into a fixed buffer being built; returns the
 * number of bytes that would have been written (snprintf semantics). */
int bsdr_json_escape(char *out, size_t outlen, const char *s);

#endif /* BSDR_JSON_H */
