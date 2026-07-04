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
/* Unit tests for protocol header check, discovery buffer, and JSON helpers. */
#include "bsdr/protocol.h"
#include "bsdr/discovery.h"
#include "bsdr/json.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s\n", msg); failures++; } \
    else printf("PASS %s\n", msg); } while (0)

int main(void) {
    /* header check */
    uint8_t bad[5] = { 0, 0, 0, 0, 0 };
    CHECK(bsdr_check_message_header(BSDR_BROADCAST_HEADER, 5), "header_match");
    CHECK(!bsdr_check_message_header(bad, 5), "header_mismatch");
    CHECK(!bsdr_check_message_header(BSDR_BROADCAST_HEADER, 4), "header_len");

    /* discovery buffer */
    bsdr_discovery_info info = {0};
    snprintf(info.session_id, sizeof(info.session_id), "sid");
    snprintf(info.version, sizeof(info.version), "0.950.2");
    snprintf(info.device_name, sizeof(info.device_name), "host");
    snprintf(info.device_id, sizeof(info.device_id), "did");
    snprintf(info.pairing_request_code, sizeof(info.pairing_request_code), "123456");

    uint8_t buf[512];
    size_t n = bsdr_discovery_build(&info, buf, sizeof(buf));
    CHECK(n > 5 && memcmp(buf, BSDR_BROADCAST_HEADER, 5) == 0, "discovery_header");

    const char *json = (const char *)buf + 5;
    char val[64];
    CHECK(bsdr_json_get_str(json, "pairingRequestCode", val, sizeof(val)) &&
          strcmp(val, "123456") == 0, "discovery_pairing_code");
    CHECK(bsdr_json_get_str(json, "deviceName", val, sizeof(val)) &&
          strcmp(val, "host") == 0, "discovery_device_name");
    CHECK(bsdr_json_get_str(json, "version", val, sizeof(val)) &&
          strcmp(val, "0.950.2") == 0, "discovery_version");

    /* JSON parse: string + number, escapes */
    const char *body = "{\"deviceName\":\"a\\\"b\",\"fps\": 90,\"x\":-1.5}";
    CHECK(bsdr_json_get_str(body, "deviceName", val, sizeof(val)) &&
          strcmp(val, "a\"b") == 0, "json_escaped_string");
    double d;
    CHECK(bsdr_json_get_double(body, "fps", &d) && d == 90.0, "json_number");
    CHECK(bsdr_json_get_double(body, "x", &d) && d == -1.5, "json_negative");
    CHECK(!bsdr_json_get_str(body, "missing", val, sizeof(val)), "json_missing");

    printf(failures ? "\nFAILED (%d)\n" : "\nOK - all protocol tests passed\n",
           failures);
    return failures ? 1 : 0;
}
