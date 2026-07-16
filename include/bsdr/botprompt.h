/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Role-adaptive system-prompt builder for the in-room bot / owner assistant.
 *
 * One place composes the LLM system prompt so it always matches what the bot is doing RIGHT NOW:
 * the caller's access level and the exact tool groups they were granted decide whether the bot is
 * framed as an owner desktop/agentic-coding assistant, a room moderator, or a friend Q&A helper —
 * and the capability notes (web search, code writing) are added only when those tools are present.
 * An operator-supplied personality description is injected as a preamble so the bot has a voice of
 * its own. Keeping this out of the call sites means the moderator, translator, computer-control and
 * friend paths can never drift apart. */
#ifndef BSDR_BOTPROMPT_H
#define BSDR_BOTPROMPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "bsdr/acl.h"   /* bsdr_acl_level + BSDR_TG_* tool-group bits */

/* Build a system prompt into `out` (always NUL-terminated). Returns the length written.
 *   mask        the tool groups the caller was granted (BSDR_TG_*) — drives the role framing
 *   lvl         the caller's resolved access level (owner/host/friend)
 *   who         the speaker's display name ("" / NULL => "someone")
 *   botname     the bot's name / wake word ("" / NULL => "the assistant")
 *   personality operator's free-text personality description (NULL / "" => none)
 *   vision      a desktop screenshot tool is available (adds the look-first note; computer only)
 *   spoken      the final reply is spoken aloud (keep it short) vs shown as text
 */
size_t bsdr_botprompt_build(char *out, size_t cap, uint32_t mask, bsdr_acl_level lvl,
                            const char *who, const char *botname, const char *personality,
                            bool vision, bool spoken);

#endif /* BSDR_BOTPROMPT_H */
