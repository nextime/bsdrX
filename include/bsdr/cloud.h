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
#include <stdint.h>

/* config.js (v0.950.2) */
#define BSDR_CLOUD_API_HOST  "main-shark-api.bigscreencloud.com"
/* Bigscreen's client API key, sent as `Authorization: Bearer <key>` on every cloud call. It is
 * Bigscreen's property, so it is BLANK in the public mirror (see the README). Supply it at runtime
 * via the BSDR_CLOUD_API_KEY environment variable; bsdr_cloud_api_key() returns that, or this
 * compiled default when the env var is unset. */
/* TWO Bigscreen app keys, used per SESSION ROLE:
 *  - COMPANION key (this one): bsdrX's own host session (the RDC companion). Its login works as-is —
 *    email/password, no x-bigscreen-system-info on /auth/login. Keep it for the host account.
 *  - CLIENT key (below): a first-class client session, the way the Bigscreen Friends app authenticates
 *    (Hermes RE) — same key on every call incl. /auth/login, WITH x-bigscreen-system-info. The
 *    second/"bot" account uses this so its sessions are client-grade and can pass the room-join gate.
 * Both are Bigscreen's property -> BLANK in the public mirror; supply at runtime via the env vars
 * BSDR_CLOUD_API_KEY (companion) and BSDR_CLOUD_CLIENT_KEY (client). */
#define BSDR_CLOUD_API_KEY_DEFAULT \
    ""
const char *bsdr_cloud_api_key(void);        /* companion key (host account) */
#define BSDR_CLOUD_CLIENT_KEY_DEFAULT \
    ""
const char *bsdr_cloud_client_key(void);     /* client key (bot account, Friends-style) */

/* The RTP/audio SSRC a peer sends under = djb2(userSessionId) — lets the bot solo one participant. */
uint32_t bsdr_cloud_user_ssrc(const char *user_session_id);
#define BSDR_CLOUD_GAME_KEY_DEFAULT BSDR_CLOUD_CLIENT_KEY_DEFAULT   /* back-compat alias */
const char *bsdr_cloud_game_key(void);
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
    int  mic_port;                     /* mediaPeer.micPort — the room's MONO voice mix (others) */
    char mic_media_ip[64];             /* media server for the mic peer (from the room-join); may
                                        * differ from media_ip. Empty => reuse media_ip. */
    char room_id[128];                 /* social roomId ("room:..."), for the room-join mic peer */
    char session_id[200];              /* mediaPeer.userSessionId */
    char legacy_user_id[64];           /* RoomUser.legacyUserId ("userNNN") — the data-channel prefix
                                        * the Quest keys remote avatars by (empty if not in the JSON) */
    int  seat_index;                   /* LocalUser.seatIndex — the bot's assigned seat; the avatar
                                        * renders/positions by it. -1 = not in the JSON (unknown). */
    char user_type[24];                /* room adminSettings.preferredUserType (Anyone/VerifiedUsersOnly/
                                        * FriendsOnly/AdminsOnly) — drives the bot-join decision tree */
    int  http_status;                  /* GET /rooms HTTP status (401/403 => token expired) */
} bsdr_cloud_screen;

/* Log in; fills `out`. Returns out->ok. `client_mode`: 0 = COMPANION (host account — companion key,
 * no x-bigscreen-system-info on login, as before); 1 = CLIENT (bot account — client key + system-info,
 * the Friends-style first-class session). */
bool bsdr_cloud_login(int client_mode, const char *email, const char *password,
                      bsdr_cloud_result *out);

/* Fetch the account profile (display name; also socialId/isVerified under the client key) with an
 * access token. `api_key` = the key that minted the token (companion for host, client for bot). */
bool bsdr_cloud_account(const char *api_key, const char *access_token, char *name, size_t name_len);
/* Like bsdr_cloud_account but returns the HTTP status: 0 = server unreachable (network), 2xx = valid,
 * 4xx = reached-but-rejected. Lets restore keep a saved session across a transient connect failure. */
int  bsdr_cloud_account_status(const char *api_key, const char *access_token, char *name, size_t name_len);

/* Renew an access token using a refresh token. `api_key` = the key that minted the session (companion
 * for host, client for bot; also selects the system-info flavor). Fills out->access_token/refresh_token. */
bool bsdr_cloud_renew(const char *api_key, const char *refresh_token, bsdr_cloud_result *out);

/* GET {cloudApiServerUrl}/rooms -> the first screen that has a mediaPeer.
 * Returns true and fills `out` (out->found) on a 2xx with a usable screen. */
bool bsdr_cloud_get_rooms(const char *access_token, bsdr_cloud_screen *out);

/* POST {cloudApiServerUrl}/room/{bareRoomId}/join -> the caller's OWN media peer in that social
 * room, whose mediaPeer carries the room-voice `micPort` (the mix of the OTHER participants). The
 * remote-desktop screen peer from /rooms does NOT expose micPort, so this extra call is how bsdrX
 * consumes the room mic (BSDR_RoomMic + the computer-control cloud fallback). `room_id` may carry the
 * "room:" prefix (stripped for the path/body). Fills out->media_ip + out->mic_port. Returns true on a
 * 2xx with a usable mic peer. NB: this registers as a room participant — call only when needed. */
bool bsdr_cloud_join_room(const char *access_token, const char *room_id, bsdr_cloud_screen *out);

/* GET {cloudApiServerUrl}/room/{RoomId}/leave — drop out of the room (undo a join). RE-confirmed from
 * the Quest client (Api.LeaveRoom): GET (not POST), verbatim RoomId (keeps the "room:" prefix), no body;
 * a null/empty id falls back to /room/current/leave. Returns the HTTP status (2xx = left). */
int bsdr_cloud_leave_room(const char *access_token, const char *room_id);

/* Quiet GET /rooms that returns ONLY the operator's current roomId (DEBUG-level logging) — for the
 * bot's follow-me poll. Returns true and fills `out` when the operator is in a room, false otherwise
 * (out set to ""). */
bool bsdr_cloud_poll_room_id(const char *access_token, char *out, size_t cap);

/* GET {cloudApiServerUrl}/room/{id} -> the full room state. The room-voice `micPort` lives on the
 * caller's OWN peer here (localUser.mediaPeer), not on the shared-screen peer /rooms returns — so
 * this is how bsdrX resolves the room mic (bsandroid does the same: join is best-effort, then
 * GET /room/{id}). Fills out->media_ip + out->mic_port from the localUser peer. Returns true on a
 * 2xx that yields a usable mic peer. */
bool bsdr_cloud_get_room(const char *access_token, const char *room_id, bsdr_cloud_screen *out);

/* GET /room/{id} and parse the FULL participant roster (not just our own peer) into `out`.
 * self_session_id (may be NULL) marks the bot's own entry. Returns the participant count (0 on
 * failure). Defined in cloud.c; the schema-robust parse lives in roster.c. */
struct bsdr_roster;
int bsdr_cloud_get_participants(const char *access_token, const char *room_id,
                               struct bsdr_roster *out, const char *self_session_id);

/* Kick a user (by their userSessionId) from a room the bot moderates: POST /room/{id}/users/{sid}/kick
 * (authoritative path). Returns the HTTP status (2xx = kicked), or -1 on a connection error. */
int bsdr_cloud_kick(const char *access_token, const char *room_id, const char *user_session_id);

/* A pending incoming friend request (from GET /social/notifications, notificationType FriendRequest). */
typedef struct { char notif_id[96]; char username[64]; char social_id[80]; } bsdr_friend_req;
/* List pending FriendRequest notifications for the account. Returns the count (fills up to cap). */
int bsdr_cloud_list_friend_requests(const char *access_token, bsdr_friend_req *out, int cap);

/* --- second-account "bot" room-join helpers (invite -> accept -> join; see bsdrx-bot-join-room-policy) --- */
/* GET /auth/account -> the caller's own socialId (needed to invite it). Returns true + fills out. */
bool bsdr_cloud_my_socialid(const char *access_token, char *out, size_t cap);
/* POST /social/notification {recipientSocialId,notificationType,version} -> invite/knock. type e.g.
 * "RoomInvite" or "FriendRequest". Returns the HTTP status (or -1). The server attaches the caller's
 * current room to a RoomInvite. */
int  bsdr_cloud_create_notification(const char *access_token, const char *recipient_social_id,
                                    const char *type);
/* GET /social/notifications -> the newest actionable RoomInvite's notificationId + metaData.roomId
 * (string-scan; bsdrX has no JSON-array parser). Returns true if one was found. */
bool bsdr_cloud_find_room_invite(const char *access_token, char *notif_id, size_t nsz,
                                 char *room_id, size_t rsz);
/* PUT /social/notification/{id}/accept {version} -> accept (this STAGES the caller's socialId in the
 * room). verb="accept" or "decline". Returns the HTTP status (or -1). */
int  bsdr_cloud_notification_action(const char *access_token, const char *notif_id, const char *verb);
/* PUT /room/{bareId} {"adminSettings":{"preferredUserType":"<type>"}} -> change the join policy
 * (owner only). Returns the HTTP status (or -1). */
int  bsdr_cloud_set_room_usertype(const char *access_token, const char *room_id, const char *usertype);

/* WS presence connection (so this host shows online and a Quest can add a screen).
 * Opens wss://main-shark-cloud/<base64(JSON{accessToken,systemInfo})> and keeps it
 * alive on a background thread until closed. */
typedef struct bsdr_cloud_ws bsdr_cloud_ws;
/* client_mode: 0 = companion system-info (host), 1 = client system-info (bot). */
bsdr_cloud_ws *bsdr_cloud_ws_open(const char *access_token, int client_mode);
void bsdr_cloud_ws_close(bsdr_cloud_ws *ws);

#endif /* BSDR_CLOUD_H */
