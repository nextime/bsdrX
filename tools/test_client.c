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
/* Stand-in headset: exercises discovery + the HTTP control API end-to-end
 * against a running bsdr_agent. Validates everything fully reversed (discovery
 * handshake + pairing + control). Does NOT drive the DTLS input channel.
 *
 *   test_client [agent-ip]   (default 127.0.0.1)
 */
#include "bsdr/platform.h"
#include "bsdr/net.h"
#include "bsdr/protocol.h"
#include "bsdr/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool discover(const char *ip, char *code, size_t codelen) {
    bsdr_socket_t s = bsdr_udp_bind("0.0.0.0", BSDR_REMOTE_CLIENT_INFO_PORT, true);
    if (s == BSDR_INVALID_SOCKET) { printf("bind 45001 failed\n"); return false; }

    struct sockaddr_in to;
    bsdr_sockaddr_make(&to, ip, BSDR_DISCOVERY_REQUEST_PORT);
    bsdr_udp_sendto(s, BSDR_BROADCAST_HEADER, BSDR_BROADCAST_HEADER_LEN, &to);

    uint8_t buf[1024];
    struct sockaddr_in from;
    int n = bsdr_udp_recvfrom(s, buf, sizeof(buf) - 1, &from);
    bsdr_socket_close(s);
    if (n <= BSDR_BROADCAST_HEADER_LEN) { printf("no discovery reply\n"); return false; }
    buf[n] = '\0';
    const char *json = (const char *)buf + BSDR_BROADCAST_HEADER_LEN;
    printf("discovered: %s\n", json);
    return bsdr_json_get_str(json, "pairingRequestCode", code, codelen);
}

/* Minimal HTTP/1.1 request; prints status line + body. Returns body via `out`. */
static bool http(const char *ip, const char *method, const char *path,
                 const char *body, char *out, size_t outlen) {
    bsdr_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in to;
    bsdr_sockaddr_make(&to, ip, BSDR_HTTP_SERVER_PORT);
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0) {
        printf("connect failed\n"); bsdr_socket_close(s); return false;
    }
    char req[1024];
    int blen = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        method, path, ip, blen, body ? body : "");
    bsdr_send_all(s, req, (size_t)n);

    char resp[2048];
    int total = 0, r;
    while ((r = (int)recv(s, resp + total, (int)(sizeof(resp) - 1 - total), 0)) > 0) {
        total += r;
        if (total >= (int)sizeof(resp) - 1) break;
    }
    bsdr_socket_close(s);
    resp[total > 0 ? total : 0] = '\0';

    char *status = resp;
    char *eol = strstr(resp, "\r\n");
    char *body_start = strstr(resp, "\r\n\r\n");
    if (eol) *eol = '\0';
    printf("%s %s -> %s\n", method, path, status);
    if (body_start && out) snprintf(out, outlen, "%s", body_start + 4);
    return body_start != NULL;
}

int main(int argc, char **argv) {
    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    if (!bsdr_platform_init()) return 1;

    char code[16] = {0};
    if (!discover(ip, code, sizeof(code))) return 1;
    printf("pairing code: %s\n", code);

    char body[256], resp[512];
    snprintf(body, sizeof(body),
             "{\"pairingRequestCode\":\"%s\",\"deviceId\":\"test-quest-001\","
             "\"deviceName\":\"TestQuest\"}", code);
    if (!http(ip, "POST", "/pair", body, resp, sizeof(resp))) return 1;

    char pid[128] = {0};
    if (!bsdr_json_get_str(resp, "pairingId", pid, sizeof(pid))) {
        printf("no pairingId in: %s\n", resp); return 1;
    }

    char path[256];
    snprintf(path, sizeof(path), "/heartbeat/%s", pid); http(ip, "GET", path, NULL, NULL, 0);
    snprintf(path, sizeof(path), "/start/%s", pid);     http(ip, "GET", path, NULL, NULL, 0);
    snprintf(path, sizeof(path), "/device/%s", pid);    http(ip, "PUT", path, "{\"fps\":90}", NULL, 0);
    bsdr_sleep_ms(300);
    snprintf(path, sizeof(path), "/stop/%s", pid);      http(ip, "GET", path, NULL, NULL, 0);
    snprintf(path, sizeof(path), "/unpair/%s", pid);    http(ip, "GET", path, NULL, NULL, 0);

    printf("OK - discovery + control round-trip passed\n");
    bsdr_platform_cleanup();
    return 0;
}
