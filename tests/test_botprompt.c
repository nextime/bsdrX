/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Role-adaptive system-prompt builder: the framing + capability notes must match the granted tool
 * groups, and the operator personality must be injected. */
#include "bsdr/botprompt.h"
#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

int main(void) {
    char p[3072];

    /* Friend: only the PUBLIC group => web search framing, NO computer/moderator text. */
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_PUBLIC, BSDR_ACL_FRIEND, "Alice", "Nova", NULL, false, true);
    CHECK(strstr(p, "web_search") != NULL, "friend_has_web_search");
    CHECK(strstr(p, "shell_exec") == NULL, "friend_no_shell");
    CHECK(strstr(p, "kick_user") == NULL, "friend_no_moderation");
    CHECK(strstr(p, "Alice") != NULL, "names_the_speaker");
    CHECK(strstr(p, "Nova")  != NULL, "names_the_bot");
    CHECK(strstr(p, "spoken aloud") != NULL, "spoken_style");

    /* Owner: full mask => computer + coding + moderation + admin + botctl. */
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_ALL, BSDR_ACL_OWNER, "owner", "Nova", NULL, true, false);
    CHECK(strstr(p, "shell_exec") != NULL, "owner_has_shell");
    CHECK(strstr(p, "write_file") != NULL, "owner_has_files");
    CHECK(strstr(p, "software engineer") != NULL, "owner_coding_framing");
    CHECK(strstr(p, "kick_user") != NULL, "owner_has_moderation");
    CHECK(strstr(p, "take_screenshot") != NULL, "owner_vision_note_when_enabled");

    /* Host moderator: MODERATOR|PUBLIC, no computer => moderation + web, no shell. */
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_MODERATOR | BSDR_TG_PUBLIC, BSDR_ACL_HOST, "Bob", "Nova", NULL, false, true);
    CHECK(strstr(p, "kick_user") != NULL, "host_has_moderation");
    CHECK(strstr(p, "shell_exec") == NULL, "host_no_shell");

    /* Browser group: only when BSDR_TG_BROWSER is in the mask (owner + opt-in). */
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_ALL, BSDR_ACL_OWNER, "owner", "Nova", NULL, false, false);
    CHECK(strstr(p, "browser_navigate") == NULL, "no_browser_unless_opted_in");
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_ALL | BSDR_TG_BROWSER, BSDR_ACL_OWNER, "owner", "Nova", NULL, false, false);
    CHECK(strstr(p, "browser_navigate") != NULL, "browser_when_opted_in");

    /* Personality injection. */
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_PUBLIC, BSDR_ACL_FRIEND, "Alice", "Nova",
                         "a grumpy pirate who loves puns", false, true);
    CHECK(strstr(p, "grumpy pirate") != NULL, "personality_injected");

    /* Vision note is suppressed when there is no computer group even if vision=true. */
    bsdr_botprompt_build(p, sizeof p, BSDR_TG_PUBLIC, BSDR_ACL_FRIEND, "Alice", "Nova", NULL, true, true);
    CHECK(strstr(p, "take_screenshot") == NULL, "no_vision_without_computer");

    printf(fail ? "\nFAILED (%d)\n" : "\nOK - botprompt passed\n", fail);
    return fail ? 1 : 0;
}
