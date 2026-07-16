/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Room participant roster + SSRC → participant map.
 *
 * Parses a Bigscreen /room JSON body into the list of people currently in the room. People live
 * under `localUser` (own peer) + `remoteUsers[]`, each carrying userSessionId / legacyUserId /
 * seatIndex / username / displayName / socialId — the authoritative RoomUserInfo schema (verified
 * from a live GET /social/rooms capture + the IL2CPP model, and cross-checked against bsdrX's
 * live-validated cloud.c). Screen mediaPeers also carry a userSessionId but are NOT people and are
 * excluded. Each entry's cloud SSRC is djb2(userSessionId) (bsdr_cloud_user_ssrc), which is how room
 * audio streams are keyed — so a decoded RTP SSRC maps straight back to a named participant, the
 * join the access-control + volume policy need. Room host = a participant whose userSessionId matches
 * a screens[].ownerUserSessionId (or whose socialId matches ownerSocialProfile.socialId). */
#ifndef BSDR_ROSTER_H
#define BSDR_ROSTER_H

#include <stdbool.h>
#include <stdint.h>

#define BSDR_ROSTER_MAX 64

typedef struct {
    char     user_session_id[200];
    char     legacy_user_id[64];
    char     username[64];
    char     social_id[80];
    int      seat_index;
    uint32_t ssrc;        /* bsdr_cloud_user_ssrc(user_session_id) */
    bool     is_self;     /* this is the bot's own peer */
    bool     is_host;     /* best-effort: this participant owns/hosts the room */
} bsdr_roster_entry;

typedef struct bsdr_roster {
    bsdr_roster_entry e[BSDR_ROSTER_MAX];
    int  n;
    bool host_known;      /* did the body expose a room host we could match to a participant? */
} bsdr_roster;

/* Reset to empty. */
void bsdr_roster_clear(bsdr_roster *r);

/* Parse a /room JSON body into `r`. self_session_id (may be NULL) marks the bot's own entry.
 * Returns the participant count. Best-effort + defensive: a body it can't parse yields n=0. */
int bsdr_roster_parse(bsdr_roster *r, const char *room_json, const char *self_session_id);

/* Lookups (return NULL if absent). */
const bsdr_roster_entry *bsdr_roster_by_ssrc(const bsdr_roster *r, uint32_t ssrc);
const bsdr_roster_entry *bsdr_roster_by_username(const bsdr_roster *r, const char *username);
const bsdr_roster_entry *bsdr_roster_host(const bsdr_roster *r);

#endif /* BSDR_ROSTER_H */
