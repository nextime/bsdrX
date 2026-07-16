/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Tiered tools: the tool→group catalog and the executor's access guard (defense-in-depth). */
#include "bsdr/llm.h"
#include "bsdr/compcontrol.h"
#include "bsdr/acl.h"

#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

int main(void) {
    /* --- tool → group catalog --- */
    CHECK(bsdr_llm_tool_group("type_text")      == BSDR_TG_COMPUTER,  "grp_type_text");
    CHECK(bsdr_llm_tool_group("shell_exec")     == BSDR_TG_COMPUTER,  "grp_shell");
    CHECK(bsdr_llm_tool_group("take_screenshot")== BSDR_TG_COMPUTER,  "grp_screenshot");
    CHECK(bsdr_llm_tool_group("web_search")     == BSDR_TG_PUBLIC,    "grp_web_search");
    CHECK(bsdr_llm_tool_group("speak")          == BSDR_TG_PUBLIC,    "grp_speak");
    CHECK(bsdr_llm_tool_group("follow_me")      == BSDR_TG_BOTCTL,    "grp_follow");
    CHECK(bsdr_llm_tool_group("authorize")      == BSDR_TG_ADMIN,     "grp_authorize");
    CHECK(bsdr_llm_tool_group("translate_next") == BSDR_TG_MODERATOR, "grp_translate");
    CHECK(bsdr_llm_tool_group("no_such_tool")   == 0,                 "grp_unknown");

    /* --- executor access guard --- */
    bsdr_compcontrol *cc = bsdr_compcontrol_new(NULL);   /* NULL inj: only test guard + safe tools */
    char res[128];

    /* a friend (PUBLIC only) must not reach a computer tool */
    bsdr_compcontrol_set_caller(cc, BSDR_TG_PUBLIC, BSDR_ACL_FRIEND, "Alice");
    bsdr_compcontrol_exec("type_text", "{\"text\":\"x\"}", res, sizeof res, cc);
    CHECK(strcmp(res, "not permitted for your access level") == 0, "friend_denied_type_text");
    bsdr_compcontrol_exec("shell_exec", "{\"command\":\"id\"}", res, sizeof res, cc);
    CHECK(strcmp(res, "not permitted for your access level") == 0, "friend_denied_shell");
    /* a friend MAY reach a public tool (stop_talking is safe with no app) */
    bsdr_compcontrol_exec("stop_talking", "{}", res, sizeof res, cc);
    CHECK(strcmp(res, "stopping") == 0, "friend_allowed_stop");

    /* the owner (all groups) passes the guard; open_app's own safety check still applies */
    bsdr_compcontrol_set_caller(cc, BSDR_TG_ALL, BSDR_ACL_OWNER, "owner");
    bsdr_compcontrol_exec("open_app", "{\"name\":\"x; rm -rf /\"}", res, sizeof res, cc);
    CHECK(strcmp(res, "rejected") == 0, "owner_open_app_metachar_rejected");
    bsdr_compcontrol_exec("bogus", "{}", res, sizeof res, cc);
    CHECK(strcmp(res, "unknown tool") == 0, "owner_unknown_tool");

    /* a host without the computer group still can't shell */
    bsdr_compcontrol_set_caller(cc, BSDR_TG_PUBLIC | BSDR_TG_MODERATOR, BSDR_ACL_HOST, "Bob");
    bsdr_compcontrol_exec("write_file", "{\"path\":\"/tmp/x\",\"content\":\"y\"}", res, sizeof res, cc);
    CHECK(strcmp(res, "not permitted for your access level") == 0, "host_denied_write_file");

    bsdr_compcontrol_free(cc);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - tiered tools passed\n", fail);
    return fail ? 1 : 0;
}
