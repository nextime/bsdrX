/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Roster parse: the authoritative localUser + remoteUsers[] schema, ssrc mapping, host detection,
 * and the mediaPeer exclusion (a screen media peer carries a userSessionId but is NOT a person). */
#include "bsdr/roster.h"
#include "bsdr/cloud.h"   /* bsdr_cloud_user_ssrc */

#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

/* Shaped after the authoritative GET /room body: localUser + remoteUsers[] + screens[] whose
 * mediaPeer also has a userSessionId (must be excluded) + screens[].ownerUserSessionId (the host). */
static const char *BODY =
"{"
"  \"roomId\":\"abc\","
"  \"ownerSocialProfile\":{\"socialId\":\"own-social\",\"username\":\"OwnerGuy\"},"
"  \"localUser\":{\"userSessionId\":\"sess-bot\",\"legacyUserId\":\"user0\",\"seatIndex\":0,\"username\":\"Aria\",\"socialId\":\"bot-social\"},"
"  \"remoteUsers\":["
"    {\"userSessionId\":\"sess-owner\",\"legacyUserId\":\"user1\",\"seatIndex\":1,\"username\":\"OwnerGuy\",\"socialId\":\"own-social\"},"
"    {\"userSessionId\":\"sess-alice\",\"legacyUserId\":\"user2\",\"seatIndex\":2,\"socialProfile\":{\"username\":\"Alice\"}}"
"  ],"
"  \"participants\":3,"
"  \"screens\":[{\"ownerUserSessionId\":\"sess-owner\",\"mediaPeer\":{\"userSessionId\":\"media-peer-xyz\",\"audioPort\":45002,\"videoPort\":45003,\"dataPort\":45004}}]"
"}";

int main(void) {
    bsdr_roster r;
    int n = bsdr_roster_parse(&r, BODY, "sess-bot");

    CHECK(n == 3, "three_people_only");   /* bot + owner + alice; media peer excluded */

    /* mediaPeer must not appear as a participant */
    bool has_media_peer = false;
    for (int i = 0; i < r.n; i++) if (strcmp(r.e[i].user_session_id, "media-peer-xyz") == 0) has_media_peer = true;
    CHECK(!has_media_peer, "mediapeer_excluded");

    /* ssrc mapping: djb2(userSessionId) → the named participant */
    const bsdr_roster_entry *alice = bsdr_roster_by_ssrc(&r, bsdr_cloud_user_ssrc("sess-alice"));
    CHECK(alice && strcmp(alice->username, "Alice") == 0, "by_ssrc_finds_alice");
    CHECK(alice && alice->seat_index == 2, "alice_seat");

    /* username nested under socialProfile is picked up */
    const bsdr_roster_entry *byname = bsdr_roster_by_username(&r, "alice");
    CHECK(byname && byname->ssrc == bsdr_cloud_user_ssrc("sess-alice"), "by_username_ci");

    /* self flag on the bot's own entry */
    const bsdr_roster_entry *self = bsdr_roster_by_ssrc(&r, bsdr_cloud_user_ssrc("sess-bot"));
    CHECK(self && self->is_self, "self_flagged");

    /* host detection via screens[].ownerUserSessionId */
    CHECK(r.host_known, "host_known");
    const bsdr_roster_entry *host = bsdr_roster_host(&r);
    CHECK(host && strcmp(host->user_session_id, "sess-owner") == 0, "host_is_owner");

    /* an unparsable body yields an empty roster, not a crash */
    bsdr_roster e;
    CHECK(bsdr_roster_parse(&e, "not json", NULL) == 0, "garbage_empty");
    CHECK(bsdr_roster_parse(&e, NULL, NULL) == 0, "null_empty");

    printf(fail ? "\nFAILED (%d)\n" : "\nOK - roster passed\n", fail);
    return fail ? 1 : 0;
}
