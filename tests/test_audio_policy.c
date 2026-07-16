/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Per-speaker room-audio volume policy: owner loud, host/friend lower, strangers silenced,
 * mic-check target audible, and the safe unity fallback when the owner isn't identifiable. */
#include "bsdr/app.h"

#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

/* Find the gain assigned to `ssrc` in a policy result (NAN-ish sentinel -1 if absent). */
static float gain_of(const uint32_t *sr, const float *gn, int m, uint32_t ssrc) {
    for (int i = 0; i < m; i++) if (sr[i] == ssrc) return gn[i];
    return -1.0f;
}

static void set_entry(bsdr_roster_entry *e, uint32_t ssrc, const char *user, const char *sid, bool host) {
    memset(e, 0, sizeof *e);
    e->ssrc = ssrc; e->seat_index = -1; e->is_host = host;
    snprintf(e->username, sizeof e->username, "%s", user);
    if (sid) snprintf(e->social_id, sizeof e->social_id, "%s", sid);
}

int main(void) {
    bsdr_app app;
    bsdr_app_init(&app);
    app.bot_solo_owner = false;   /* init defaults this ON ("listen only to me"); test the graded policy */
    bsdr_acl_set_owner(app.acl, "own-sid", "Owner");
    bsdr_acl_friend_add(app.acl, "fr-sid", "Alice");

    /* roster: owner(1000), friend Alice(2000), stranger Bob(3000) */
    app.roster.n = 3;
    set_entry(&app.roster.e[0], 1000, "Owner",  "own-sid", true);   /* owner is also the room host */
    set_entry(&app.roster.e[1], 2000, "Alice",  "fr-sid",  false);
    set_entry(&app.roster.e[2], 3000, "Bob",    "str-sid", false);

    uint32_t sr[BSDR_ROSTER_MAX]; float gn[BSDR_ROSTER_MAX], dflt = -1.0f;
    int m = bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt);
    CHECK(m == 3, "policy_count");
    CHECK(gain_of(sr, gn, m, 1000) == 1.0f, "owner_loud");
    CHECK(gain_of(sr, gn, m, 2000) == 0.7f, "friend_lower");
    CHECK(gain_of(sr, gn, m, 3000) == 0.0f, "stranger_muted");
    CHECK(dflt == 0.0f, "default_muted");

    /* a friend hosting the room -> still 0.7 (host tier), owner still 1.0 */
    set_entry(&app.roster.e[1], 2000, "Alice", "fr-sid", true);   /* Alice now hosts */
    m = bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt);
    CHECK(gain_of(sr, gn, m, 2000) == 0.7f, "host_friend_lower");
    set_entry(&app.roster.e[1], 2000, "Alice", "fr-sid", false);

    /* "listen only to me": friends drop to 0, owner stays 1.0 */
    app.bot_solo_owner = true;
    m = bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt);
    CHECK(gain_of(sr, gn, m, 1000) == 1.0f, "solo_owner_still_loud");
    CHECK(gain_of(sr, gn, m, 2000) == 0.0f, "solo_owner_mutes_friend");
    app.bot_solo_owner = false;

    /* mic check: the target stranger becomes audible even while muted-by-default */
    app.mic_check_ssrc = 3000;
    m = bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt);
    CHECK(gain_of(sr, gn, m, 3000) == 1.0f, "miccheck_target_audible");
    app.mic_check_ssrc = 0;

    /* the bot's own entry is never looped back */
    app.roster.e[2].is_self = true;   /* pretend Bob's slot is the bot */
    m = bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt);
    CHECK(gain_of(sr, gn, m, 3000) == -1.0f, "self_excluded");
    app.roster.e[2].is_self = false;

    /* no identifiable owner -> -1 (caller keeps unity; never silences the room blindly) */
    bsdr_acl_set_owner(app.acl, "someone-else", "Nobody");
    m = bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt);
    CHECK(m == -1, "no_owner_fallback");

    /* empty roster -> -1 */
    app.roster.n = 0;
    CHECK(bsdr_app_audio_gains(&app, sr, gn, BSDR_ROSTER_MAX, &dflt) == -1, "empty_fallback");

    bsdr_app_free(&app);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - audio policy passed\n", fail);
    return fail ? 1 : 0;
}
