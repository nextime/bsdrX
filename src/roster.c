/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Room participant roster — see bsdr/roster.h. */
#include "bsdr/roster.h"
#include "bsdr/cloud.h"    /* bsdr_cloud_user_ssrc */
#include "bsdr/log.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void bsdr_roster_clear(bsdr_roster *r) {
    if (r) { r->n = 0; r->host_known = false; }
}

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

/* Read a participant object into an entry. Fields not present are left empty. Returns false if the
 * object has no userSessionId (not a participant). */
static bool read_user(const cJSON *o, bsdr_roster_entry *e, const char *self_sid) {
    const char *usid = jstr(o, "userSessionId");
    if (!usid || !usid[0]) return false;
    memset(e, 0, sizeof *e);
    e->seat_index = -1;
    snprintf(e->user_session_id, sizeof e->user_session_id, "%s", usid);
    const char *lid = jstr(o, "legacyUserId");
    if (lid) snprintf(e->legacy_user_id, sizeof e->legacy_user_id, "%s", lid);
    /* username: a direct field, or nested under socialProfile (the shape the social API uses) */
    const char *un = jstr(o, "username");
    if (!un) un = jstr(o, "displayName");
    const cJSON *prof = cJSON_GetObjectItemCaseSensitive(o, "socialProfile");
    if (!un && cJSON_IsObject(prof)) { un = jstr(prof, "username"); if (!un) un = jstr(prof, "displayName"); }
    if (un) snprintf(e->username, sizeof e->username, "%s", un);
    /* socialId: direct or nested under socialProfile (may be absent in the room body) */
    const char *sid = jstr(o, "socialId");
    if (!sid && cJSON_IsObject(prof)) sid = jstr(prof, "socialId");
    if (sid) snprintf(e->social_id, sizeof e->social_id, "%s", sid);
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(o, "seatIndex");
    if (cJSON_IsNumber(si)) e->seat_index = si->valueint;
    e->ssrc = bsdr_cloud_user_ssrc(usid);
    e->is_self = self_sid && self_sid[0] && strcmp(usid, self_sid) == 0;
    return true;
}

/* Already have this userSessionId? (localUser can also appear inside an array). */
static bool have_usid(const bsdr_roster *r, const char *usid) {
    for (int i = 0; i < r->n; i++) if (strcmp(r->e[i].user_session_id, usid) == 0) return true;
    return false;
}

/* Add one participant object if it looks like a PERSON. The authoritative room body (verified from a
 * live GET /social/rooms capture + IL2CPP RoomUserInfo model) has people under `localUser` +
 * `remoteUsers[]`, each carrying userSessionId/legacyUserId/seatIndex/username/displayName. Objects
 * like screens[].mediaPeer ALSO carry a userSessionId (+ audio/video/dataPort) but are NOT people —
 * require_person gates those out by demanding a person-ish field (username/displayName/seatIndex). */
static void add_user(const cJSON *o, bsdr_roster *r, const char *self_sid, bool require_person) {
    if (!cJSON_IsObject(o) || r->n >= BSDR_ROSTER_MAX) return;
    const char *usid = jstr(o, "userSessionId");
    if (!usid || !usid[0] || have_usid(r, usid)) return;
    if (require_person) {
        bool person = jstr(o, "username") || jstr(o, "displayName") ||
                      cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, "seatIndex")) ||
                      cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(o, "socialProfile"));
        if (!person) return;
    }
    bsdr_roster_entry tmp;
    if (read_user(o, &tmp, self_sid)) r->e[r->n++] = tmp;
}

/* Room-host detection from the AUTHORITATIVE owner fields (there is no hostId/ownerSocialId scalar):
 *   - screens[].ownerUserSessionId  → match a participant's userSessionId
 *   - ownerSocialProfile.socialId / creatorSocialId → match a participant's socialId
 * If none matches, host stays unknown and callers treat friends as plain Friends (safe default). */
static void mark_host_by_usid(bsdr_roster *r, const char *usid) {
    if (!usid || !usid[0]) return;
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->e[i].user_session_id, usid) == 0) { r->e[i].is_host = true; r->host_known = true; return; }
}
static void mark_host_by_sid(bsdr_roster *r, const char *sid) {
    if (!sid || !sid[0]) return;
    for (int i = 0; i < r->n; i++)
        if (r->e[i].social_id[0] && strcmp(r->e[i].social_id, sid) == 0) { r->e[i].is_host = true; r->host_known = true; return; }
}
static void detect_host(const cJSON *root, bsdr_roster *r) {
    const cJSON *screens = cJSON_GetObjectItemCaseSensitive(root, "screens");
    const cJSON *sc;
    cJSON_ArrayForEach(sc, screens) {
        mark_host_by_usid(r, jstr(sc, "ownerUserSessionId"));
        if (r->host_known) return;
    }
    const cJSON *osp = cJSON_GetObjectItemCaseSensitive(root, "ownerSocialProfile");
    if (cJSON_IsObject(osp)) { mark_host_by_sid(r, jstr(osp, "socialId")); if (r->host_known) return; }
    mark_host_by_sid(r, jstr(root, "creatorSocialId"));
}

int bsdr_roster_parse(bsdr_roster *r, const char *room_json, const char *self_session_id) {
    if (!r) return 0;
    bsdr_roster_clear(r);
    if (!room_json || !room_json[0]) return 0;
    cJSON *root = cJSON_Parse(room_json);
    if (!root) { BSDR_DEBUG("bsdr.roster", "room body did not parse as JSON"); return 0; }
    /* People live under localUser (own peer) + remoteUsers[] (authoritative keys). */
    add_user(cJSON_GetObjectItemCaseSensitive(root, "localUser"), r, self_session_id, false);
    const cJSON *ru = cJSON_GetObjectItemCaseSensitive(root, "remoteUsers"), *it;
    cJSON_ArrayForEach(it, ru) add_user(it, r, self_session_id, false);
    /* Fallback for join-response shapes that don't use those keys: a guarded walk that only accepts
     * person-ish objects (so screen mediaPeers with a userSessionId are not mistaken for people). */
    if (r->n == 0)
        for (const cJSON *c = root ? root->child : NULL; c; c = c->next) {
            if (cJSON_IsArray(c)) { const cJSON *e; cJSON_ArrayForEach(e, c) add_user(e, r, self_session_id, true); }
            else if (cJSON_IsObject(c)) add_user(c, r, self_session_id, true);
        }
    detect_host(root, r);
    cJSON_Delete(root);
    BSDR_DEBUG("bsdr.roster", "parsed %d participant(s), host_known=%d", r->n, r->host_known);
    return r->n;
}

const bsdr_roster_entry *bsdr_roster_by_ssrc(const bsdr_roster *r, uint32_t ssrc) {
    if (!r || !ssrc) return NULL;
    for (int i = 0; i < r->n; i++) if (r->e[i].ssrc == ssrc) return &r->e[i];
    return NULL;
}
const bsdr_roster_entry *bsdr_roster_by_username(const bsdr_roster *r, const char *username) {
    if (!r || !username || !username[0]) return NULL;
    for (int i = 0; i < r->n; i++) if (r->e[i].username[0] && strcasecmp(r->e[i].username, username) == 0) return &r->e[i];
    return NULL;
}
const bsdr_roster_entry *bsdr_roster_host(const bsdr_roster *r) {
    if (!r) return NULL;
    for (int i = 0; i < r->n; i++) if (r->e[i].is_host) return &r->e[i];
    return NULL;
}
