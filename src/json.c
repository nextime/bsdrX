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
#include "bsdr/json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* p points at an opening '"'; returns the pointer just past the closing '"',
 * honoring backslash escapes. */
static const char *skip_string(const char *p) {
    const char *q = p + 1;
    while (*q) {
        if (*q == '\\' && q[1]) { q += 2; continue; }
        if (*q == '"') return q + 1;
        q++;
    }
    return q;
}

/* Find the value position for top-level key "key". Returns pointer just after
 * the ':' or NULL. Handles quotes/escapes inside string values so they don't
 * desync key matching (good enough for our flat control bodies). */
static const char *find_value(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while (*p) {
        if (*p != '"') { p++; continue; }
        const char *kstart = p + 1;
        const char *kend = skip_string(p);          /* just past closing quote */
        const char *c = kend;
        while (*c && isspace((unsigned char)*c)) c++;
        if (*c != ':') { p = kend; continue; }       /* a value string, not a key */
        c++;
        while (*c && isspace((unsigned char)*c)) c++;
        if ((size_t)(kend - 1 - kstart) == klen &&
            strncmp(kstart, key, klen) == 0)
            return c;
        /* not our key: skip its value (skip string values wholesale so their
         * inner quotes don't confuse us; scalars have no quotes). */
        p = (*c == '"') ? skip_string(c) : c + 1;
    }
    return NULL;
}

bool bsdr_json_get_str(const char *json, const char *key,
                       char *out, size_t outlen) {
    if (!json || !out || outlen == 0) return false;
    const char *v = find_value(json, key);
    if (!v || *v != '"') return false;
    v++;
    size_t o = 0;
    while (*v && *v != '"') {
        char ch = *v;
        if (ch == '\\' && v[1]) {
            v++;
            switch (*v) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '"': ch = '"';  break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/';  break;
                default: ch = *v; break;
            }
        }
        if (o + 1 >= outlen) break;
        out[o++] = ch;
        v++;
    }
    out[o] = '\0';
    return true;
}

bool bsdr_json_get_double(const char *json, const char *key, double *out) {
    const char *v = find_value(json, key);
    if (!v) return false;
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return false;
    if (out) *out = d;
    return true;
}

int bsdr_json_escape(char *out, size_t outlen, const char *s) {
    size_t o = 0;
    for (const char *p = s; *p; p++) {
        const char *esc = NULL;
        char buf[3] = { '\\', 0, 0 };
        switch (*p) {
            case '"':  buf[1] = '"';  esc = buf; break;
            case '\\': buf[1] = '\\'; esc = buf; break;
            case '\n': buf[1] = 'n';  esc = buf; break;
            case '\t': buf[1] = 't';  esc = buf; break;
            case '\r': buf[1] = 'r';  esc = buf; break;
            default: break;
        }
        if (esc) {
            if (o + 2 < outlen) { out[o] = esc[0]; out[o + 1] = esc[1]; }
            o += 2;
        } else if ((unsigned char)*p < 0x20) {
            /* Other control bytes (\b, 0x01, …) must be \u00XX — a raw control char is invalid
             * JSON and can break a strict parser downstream. */
            static const char HEX[] = "0123456789abcdef";
            unsigned char ch = (unsigned char)*p;
            char u[6] = { '\\', 'u', '0', '0', HEX[ch >> 4], HEX[ch & 0xF] };
            if (o + 6 < outlen) for (int k = 0; k < 6; k++) out[o + k] = u[k];
            o += 6;
        } else {
            if (o + 1 < outlen) out[o] = *p;
            o += 1;
        }
    }
    if (outlen) out[o < outlen ? o : outlen - 1] = '\0';
    return (int)o;
}
