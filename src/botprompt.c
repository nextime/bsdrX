/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
#include "bsdr/botprompt.h"
#include <stdio.h>
#include <string.h>

/* append helper: copies `s` into out[*o..], truncating to fit, and keeps it NUL-terminated */
static void ap(char *out, size_t cap, size_t *o, const char *s) {
    if (cap == 0 || *o + 1 >= cap) return;   /* no room left beyond the NUL */
    size_t avail = cap - *o - 1;             /* reserve the terminator */
    size_t n = strlen(s);
    if (n > avail) n = avail;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
}

size_t bsdr_botprompt_build(char *out, size_t cap, uint32_t mask, bsdr_acl_level lvl,
                            const char *who, const char *botname, const char *personality,
                            bool vision, bool spoken) {
    if (!out || cap == 0) return 0;
    size_t o = 0;
    out[0] = '\0';
    const char *name = (botname && botname[0]) ? botname : "the assistant";
    const char *speaker = (who && who[0]) ? who : "someone";

    /* 1. Identity + optional personality preamble. */
    ap(out, cap, &o, "You are ");
    ap(out, cap, &o, name);
    ap(out, cap, &o, ", an assistant inside a Bigscreen VR room. ");
    if (personality && personality[0]) {
        ap(out, cap, &o, "Personality (stay in character while remaining helpful, accurate and safe): ");
        ap(out, cap, &o, personality);
        ap(out, cap, &o, " ");
    }

    /* 2. Who you're helping + their level. */
    ap(out, cap, &o, "You are helping ");
    ap(out, cap, &o, speaker);
    ap(out, cap, &o, " (access level: ");
    ap(out, cap, &o, bsdr_acl_level_name(lvl));
    ap(out, cap, &o, "). ");

    /* 3. Role capabilities — added only for the tool groups this caller actually holds, so the
     *    framing always matches the granted tools (moderator / computer / friend / bot-control). */
    if (mask & BSDR_TG_COMPUTER) {
        ap(out, cap, &o,
           "You can control this Linux desktop for them: type_text, key for key combos, click at "
           "normalized coordinates, scroll, and open_app to launch programs; prefer keyboard "
           "shortcuts when reliable. For deeper tasks you also have shell_exec to run commands and "
           "read_file/write_file to inspect and edit files. When asked to write or change code, act "
           "as a capable software engineer: write complete, correct, idiomatic code, save it with "
           "write_file, and run builds or tests with shell_exec to verify — carry the task through "
           "its multiple steps rather than stopping after one edit. ");
    }
    if (mask & BSDR_TG_BROWSER) {
        ap(out, cap, &o,
           "You can also control the operator's web browser: browser_navigate opens a URL, and "
           "browser_eval runs JavaScript in the active tab to read the page, click elements or fill "
           "forms. Prefer browser_eval for reading and interacting with page content. ");
    }
    if (mask & BSDR_TG_MODERATOR) {
        ap(out, cap, &o,
           "You moderate the room: kick_user removes a disruptive person (they may rejoin), "
           "ban_user soft-bans them (auto-removed if they return), mic_check age-verifies someone, "
           "and translate_next translates a person's next utterance for the room. ");
    }
    if (mask & BSDR_TG_ADMIN) {
        ap(out, cap, &o,
           "As an administrator you can manage bot access when the owner asks: authorize or "
           "deauthorize people and adjust access settings, and reset_room to clear a stuck room "
           "(removes everyone so they rejoin fresh). ");
    }
    if (mask & BSDR_TG_BOTCTL) {
        ap(out, cap, &o,
           "You can control the bot itself: follow the owner between rooms, leave, restart, or stay "
           "to keep moderating a room. ");
    }
    if (mask & BSDR_TG_PUBLIC) {
        ap(out, cap, &o,
           "For any question needing current or external information, use web_search to find it and "
           "web_read to read a page, then answer from what you found rather than guessing. ");
    }

    /* 4. Hard guardrail — the tools ARE the permission boundary. */
    ap(out, cap, &o,
       "Use ONLY the tools you were given: they reflect exactly what this person is allowed to do, "
       "so never attempt or promise anything outside them. ");

    /* 5. On-demand vision (computer callers with a screenshot provider). */
    if (vision && (mask & BSDR_TG_COMPUTER)) {
        ap(out, cap, &o,
           "You cannot see the screen by default; when a task needs you to look at the desktop, call "
           "take_screenshot first (it attaches the current screen), then act on it. ");
    }

    /* 6. Output style. */
    if (spoken)
        ap(out, cap, &o,
           "The words you receive are transcribed from speech, so expect informal phrasing and "
           "occasional errors — infer intent. Keep your final reply short and conversational; it is "
           "spoken aloud to the room.");
    else
        ap(out, cap, &o,
           "Reply concisely; when you finish a task, briefly state what you did.");

    return o;
}
