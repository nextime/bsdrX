/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Access-control: level resolution, the tool-group matrix, friend/ban tables, persistence. */
#include "bsdr/acl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

int main(void) {
    bsdr_acl *a = bsdr_acl_new();
    bsdr_acl_set_owner(a, "own-sid", "Owner");
    bsdr_acl_set_bot(a, "bot-sid", "Aria");
    bsdr_acl_friend_add(a, "fr-sid", "Alice");

    /* --- resolution --------------------------------------------------------- */
    CHECK(bsdr_acl_resolve(a, "own-sid", NULL, false) == BSDR_ACL_OWNER, "owner_by_socialid");
    CHECK(bsdr_acl_resolve(a, NULL, "Owner", true) == BSDR_ACL_OWNER,    "owner_by_username");
    CHECK(bsdr_acl_resolve(a, "fr-sid", NULL, false) == BSDR_ACL_FRIEND, "friend_default");
    CHECK(bsdr_acl_resolve(a, NULL, "alice", false) == BSDR_ACL_FRIEND,  "friend_by_username_ci");
    CHECK(bsdr_acl_resolve(a, "fr-sid", NULL, true) == BSDR_ACL_HOST,    "friend_hosting_promotes");
    CHECK(bsdr_acl_resolve(a, "x-sid", "Bob", true) == BSDR_ACL_NONE,    "stranger_none");

    /* --- toggles gate resolution ------------------------------------------- */
    bsdr_acl_set_host_access(a, false);
    CHECK(bsdr_acl_resolve(a, "fr-sid", NULL, true) == BSDR_ACL_FRIEND, "host_access_off_demotes_to_friend");
    bsdr_acl_set_host_access(a, true);
    bsdr_acl_set_friend_access(a, false);
    CHECK(bsdr_acl_resolve(a, "fr-sid", NULL, false) == BSDR_ACL_NONE,  "friend_access_off_denies");
    CHECK(bsdr_acl_resolve(a, "own-sid", NULL, false) == BSDR_ACL_OWNER, "owner_unaffected_by_toggles");
    bsdr_acl_set_friend_access(a, true);

    /* --- tool-group matrix -------------------------------------------------- */
    CHECK(bsdr_acl_toolmask(a, BSDR_ACL_OWNER, true) == BSDR_TG_ALL, "owner_all_tools");
    CHECK(bsdr_acl_toolmask(a, BSDR_ACL_FRIEND, false) == BSDR_TG_PUBLIC, "friend_public_only");
    CHECK(bsdr_acl_toolmask(a, BSDR_ACL_HOST, true) == (BSDR_TG_PUBLIC|BSDR_TG_MODERATOR|BSDR_TG_BOTCTL),
          "host_own_room_mod_botctl");
    CHECK(bsdr_acl_toolmask(a, BSDR_ACL_HOST, false) == (BSDR_TG_PUBLIC|BSDR_TG_MODERATOR),
          "host_other_room_no_botctl");
    CHECK(bsdr_acl_toolmask(a, BSDR_ACL_NONE, true) == 0, "none_no_tools");

    /* a globally-disabled group is removed even for the owner */
    bsdr_acl_set_group_enabled(a, BSDR_TG_COMPUTER, false);
    CHECK((bsdr_acl_toolmask(a, BSDR_ACL_OWNER, true) & BSDR_TG_COMPUTER) == 0, "group_disable_kills_computer");
    CHECK((bsdr_acl_toolmask(a, BSDR_ACL_OWNER, true) & BSDR_TG_ADMIN) != 0,   "group_disable_leaves_others");
    bsdr_acl_set_group_enabled(a, BSDR_TG_COMPUTER, true);

    /* --- friend / ban tables ------------------------------------------------ */
    CHECK(bsdr_acl_is_friend(a, "fr-sid", NULL), "is_friend");
    CHECK(bsdr_acl_friend_remove(a, "Alice"), "friend_remove_by_username");
    CHECK(!bsdr_acl_is_friend(a, "fr-sid", NULL), "friend_removed");
    bsdr_acl_ban_add(a, "ban-sid", "Troll");
    CHECK(bsdr_acl_is_banned(a, NULL, "troll"), "is_banned_ci");
    CHECK(bsdr_acl_ban_remove(a, "ban-sid"), "ban_remove");
    CHECK(!bsdr_acl_is_banned(a, "ban-sid", NULL), "ban_removed");

    /* --- persistence round-trip -------------------------------------------- */
    bsdr_acl_friend_add(a, "p-sid", "Persist");
    bsdr_acl_ban_add(a, "pb-sid", "PBan");
    bsdr_acl_set_group_enabled(a, BSDR_TG_ADMIN, false);
    const char *path = "acl_test.json";
    CHECK(bsdr_acl_save(a, path), "save");

    bsdr_acl *b = bsdr_acl_new();
    CHECK(bsdr_acl_load(b, path), "load");
    CHECK(bsdr_acl_is_friend(b, "p-sid", NULL), "reload_friend");
    CHECK(bsdr_acl_is_banned(b, "pb-sid", NULL), "reload_ban");
    CHECK((bsdr_acl_group_mask(b) & BSDR_TG_ADMIN) == 0, "reload_group_mask");
    CHECK(bsdr_acl_resolve(b, "own-sid", NULL, false) == BSDR_ACL_OWNER, "reload_owner_identity");
    remove(path);

    /* --- migration: an access.json from before the wake word moved to the fullbot plugin ---------
     * The stale key must be ignored, not abort the load and cost the owner their friends + bans. */
    FILE *of = fopen(path, "w");
    CHECK(of != NULL, "old_json_open");
    if (of) {
        fputs("{\"owner\":{\"socialId\":\"old-sid\",\"username\":\"Old\"},"
              "\"wakeWord\":\"Jarvis\",\"friendAccess\":true,"
              "\"friends\":[{\"socialId\":\"of-sid\",\"username\":\"OldFriend\"}]}", of);
        fclose(of);
    }
    bsdr_acl *c = bsdr_acl_new();
    CHECK(bsdr_acl_load(c, path), "old_json_loads");
    CHECK(bsdr_acl_is_friend(c, "of-sid", NULL), "old_json_keeps_friends");
    CHECK(bsdr_acl_resolve(c, "old-sid", NULL, false) == BSDR_ACL_OWNER, "old_json_keeps_owner");
    bsdr_acl_free(c);
    remove(path);

    bsdr_acl_free(a);
    bsdr_acl_free(b);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - acl passed\n", fail);
    return fail ? 1 : 0;
}
