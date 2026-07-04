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
/* Bigscreen cloud client (Internet path): HTTPS login to main-shark-api.
 * POST /auth/login  Authorization: Bearer <apiKey>, {email,password} ->
 * x-access-token / x-refresh-token response headers. Account profile via
 * GET /auth/account. (Testable end-to-end only with a real Bigscreen account.) */
#ifndef BSDR_CLOUD_H
#define BSDR_CLOUD_H

#include <stdbool.h>
#include <stddef.h>

/* config.js (v0.950.2) */
#define BSDR_CLOUD_API_HOST  "main-shark-api.bigscreencloud.com"
/* Bigscreen's client API key, sent as `Authorization: Bearer <key>` on every cloud call. It is
 * Bigscreen's property, so it is BLANK in the public mirror (see the README). Supply it at runtime
 * via the BSDR_CLOUD_API_KEY environment variable; bsdr_cloud_api_key() returns that, or this
 * compiled default when the env var is unset. */
#define BSDR_CLOUD_API_KEY_DEFAULT \
    ""
const char *bsdr_cloud_api_key(void);
#define BSDR_CLOUD_WS_HOST   "main-shark-cloud.bigscreencloud.com"
#define BSDR_CLOUD_API2_HOST "main-shark-cloud-api.bigscreencloud.com"  /* cloudApiServerUrl */

typedef struct {
    bool ok;
    int http_status;
    char access_token[2048];
    char refresh_token[2048];
    char message[256];      /* error/status text */
} bsdr_cloud_result;

/* A Mediasoup relay assignment for one shared screen (from GET /rooms). */
typedef struct {
    bool found;
    char media_ip[64];                 /* mediaServer.ipAddress */
    int  video_port, audio_port, data_port;   /* mediaPeer.{video,audio,data}Port */
    char session_id[200];              /* mediaPeer.userSessionId */
    int  http_status;                  /* GET /rooms HTTP status (401/403 => token expired) */
} bsdr_cloud_screen;

/* Log in; fills `out`. Returns out->ok. */
bool bsdr_cloud_login(const char *email, const char *password,
                      bsdr_cloud_result *out);

/* Fetch the account profile (display name) with an access token. */
bool bsdr_cloud_account(const char *access_token, char *name, size_t name_len);

/* Renew an access token using a refresh token (POST /auth/renew, x-refresh-token header +
 * x-bigscreen-system-info). Fills out->access_token/refresh_token. Returns out->ok. */
bool bsdr_cloud_renew(const char *refresh_token, bsdr_cloud_result *out);

/* GET {cloudApiServerUrl}/rooms -> the first screen that has a mediaPeer.
 * Returns true and fills `out` (out->found) on a 2xx with a usable screen. */
bool bsdr_cloud_get_rooms(const char *access_token, bsdr_cloud_screen *out);

/* WS presence connection (so this host shows online and a Quest can add a screen).
 * Opens wss://main-shark-cloud/<base64(JSON{accessToken,systemInfo})> and keeps it
 * alive on a background thread until closed. */
typedef struct bsdr_cloud_ws bsdr_cloud_ws;
bsdr_cloud_ws *bsdr_cloud_ws_open(const char *access_token);
void bsdr_cloud_ws_close(bsdr_cloud_ws *ws);

#endif /* BSDR_CLOUD_H */
