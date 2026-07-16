/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Moderation glue: owner-SSRC resolution + soft-ban persistence (kick itself needs the network, so
 * offline it fails, but ban_user must still record the socialId so a rejoin is auto-re-kicked). */
#define _GNU_SOURCE
#include "bsdr/app.h"
#include "bsdr/roomcmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

static void se(bsdr_roster_entry *e, uint32_t s, const char *u, const char *sid, int h) {
    memset(e, 0, sizeof *e); e->ssrc = s; e->seat_index = -1; e->is_host = h;
    snprintf(e->username, sizeof e->username, "%s", u);
    if (sid) snprintf(e->social_id, sizeof e->social_id, "%s", sid);
    snprintf(e->user_session_id, sizeof e->user_session_id, "sess-%s", u);
}

int main(void) {
    /* Isolate persistence from the real config dir — bsdr_app_ban_user saves access.json, and
     * bsdr_app_save_settings writes `settings`. config_dir only mkdir's one level, so the parent
     * must already exist. */
    mkdir("/tmp/bsdr-test-cfg", 0700);
    setenv("XDG_CONFIG_HOME", "/tmp/bsdr-test-cfg", 1);
    bsdr_app app;
    bsdr_app_init(&app);
    bsdr_acl_set_owner(app.acl, "own-sid", "Owner");
    bsdr_acl_friend_add(app.acl, "fr-sid", "Alice");
    app.roster.n = 3;
    se(&app.roster.e[0], 1000, "Owner", "own-sid", 1);
    se(&app.roster.e[1], 2000, "Alice", "fr-sid", 0);
    se(&app.roster.e[2], 3000, "Bob",   "bob-sid", 0);

    /* owner-ssrc resolution (drives overload protection) */
    CHECK(bsdr_app_owner_ssrc(&app) == 1000, "owner_ssrc");

    /* ban a present stranger: kick fails offline (no room/token), but the ban must persist by socialId */
    bsdr_app_ban_user(&app, "Bob");
    CHECK(bsdr_acl_is_banned(app.acl, "bob-sid", "Bob"), "ban_persisted_by_socialid");
    CHECK(bsdr_acl_is_banned(app.acl, NULL, "bob"), "ban_matches_ci_username");

    /* kicking someone not in the room fails cleanly */
    CHECK(bsdr_app_kick_user(&app, "Nobody") == false, "kick_unknown_fails");

    /* mic_check_auto + web-search config survive a restart (flat settings file) */
    app.mic_check_auto = true;
    snprintf(app.web_search_endpoint, sizeof app.web_search_endpoint, "https://search.example/api");
    bsdr_app_save_settings(&app);
    bsdr_app app2; bsdr_app_init(&app2);
    bsdr_app_load_settings(&app2);
    CHECK(app2.mic_check_auto == true, "mic_check_auto_persists");
    CHECK(strcmp(app2.web_search_endpoint, "https://search.example/api") == 0, "web_search_persists");
    bsdr_app_free(&app2);

    /* auto mic-check: arms once, then self-limits (dedup + one-at-a-time). TTS is off so nothing is
     * spoken; the ~20 s no-response timeout thread never fires (the process exits first), so rc is
     * intentionally not freed. */
    bsdr_roomcmd *rc = bsdr_roomcmd_new(&app);
    bsdr_roomcmd_set_enabled(rc, 1);
    CHECK(bsdr_roomcmd_autocheck(rc, 3000, "Bob")   == 1, "autocheck_first_arms");
    CHECK(bsdr_roomcmd_autocheck(rc, 3000, "Bob")   == 0, "autocheck_no_double");
    CHECK(bsdr_roomcmd_autocheck(rc, 4000, "Carol") == 0, "autocheck_one_at_a_time");

    bsdr_app_free(&app);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - moderation passed\n", fail);
    return fail ? 1 : 0;
}
