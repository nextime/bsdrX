/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Conversation context + compaction (pure string logic, no network). */
#include "bsdr/llmctx.h"
#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

static void seed(bsdr_llmctx *c, int tool_pairs) {
    bsdr_llmctx_push(c, BSDR_MSG_SYSTEM, "{\"role\":\"system\",\"content\":\"sys\"}");
    bsdr_llmctx_push(c, BSDR_MSG_USER,   "{\"role\":\"user\",\"content\":\"do a big task\"}");
    for (int i = 0; i < tool_pairs; i++) {
        char m[128];
        snprintf(m, sizeof m, "{\"role\":\"assistant\",\"content\":\"step%d\"}", i);
        bsdr_llmctx_push(c, BSDR_MSG_ASSISTANT, m);
        snprintf(m, sizeof m, "{\"role\":\"tool\",\"tool_call_id\":\"t%d\",\"content\":\"res%d\"}", i, i);
        bsdr_llmctx_push(c, BSDR_MSG_TOOL, m);
    }
}

int main(void) {
    /* render + count */
    bsdr_llmctx *c = bsdr_llmctx_new();
    seed(c, 5);                       /* 2 + 10 = 12 messages */
    CHECK(bsdr_llmctx_count(c) == 12, "count");
    char buf[4096];
    size_t n = bsdr_llmctx_render(c, 0, buf, sizeof buf);
    CHECK(n > 0, "render_nonzero");
    CHECK(strstr(buf, "\"sys\"") && strstr(buf, "step0") && strstr(buf, "res4"), "render_has_all");
    /* commas join messages: exactly count-1 top-level separators is hard to count naively, but the
     * rendered blob must start with '{' and not with a comma */
    CHECK(buf[0] == '{', "render_no_leading_comma");

    /* drop_range keeps system+user (2) + tail */
    int first = -1, last = -1;
    CHECK(bsdr_llmctx_drop_range(c, 4, &first, &last), "drop_range_true");
    CHECK(first == 2 && last == 12 - 4 - 1, "drop_range_bounds");   /* [2 .. 7] */

    /* range render is a sub-slice only */
    char rb[2048];
    bsdr_llmctx_render_range(c, first, last, rb, sizeof rb);
    CHECK(strstr(rb, "step0") && !strstr(rb, "\"sys\""), "render_range_excludes_head");

    /* TRUNCATE: drops the middle, keeps 2 head + 4 tail = 6 */
    int after = bsdr_llmctx_compact(c, BSDR_COMPACT_TRUNCATE, 4, NULL);
    CHECK(after == 6, "truncate_count");
    bsdr_llmctx_render(c, 0, buf, sizeof buf);
    CHECK(strstr(buf, "\"sys\"") && strstr(buf, "do a big task"), "truncate_keeps_head");
    CHECK(!strstr(buf, "step0") && strstr(buf, "step4"), "truncate_drops_old_keeps_recent");
    bsdr_llmctx_free(c);

    /* SUMMARY: inserts one summary system message after the first user turn */
    bsdr_llmctx *c2 = bsdr_llmctx_new();
    seed(c2, 5);
    int a2 = bsdr_llmctx_compact(c2, BSDR_COMPACT_SUMMARY, 4, "did steps 0-3, edited foo.c");
    CHECK(a2 == 7, "summary_count");   /* 2 head + 1 summary + 4 tail */
    bsdr_llmctx_render(c2, 0, buf, sizeof buf);
    CHECK(strstr(buf, "did steps 0-3") && strstr(buf, "earlier conversation summary"), "summary_inserted");
    bsdr_llmctx_free(c2);

    /* nothing to drop when short */
    bsdr_llmctx *c3 = bsdr_llmctx_new();
    seed(c3, 1);                       /* 4 messages */
    CHECK(bsdr_llmctx_compact(c3, BSDR_COMPACT_TRUNCATE, 8, NULL) == 4, "no_drop_when_small");
    bsdr_llmctx_free(c3);

    printf(fail ? "\nFAILED (%d)\n" : "\nOK - llmctx passed\n", fail);
    return fail ? 1 : 0;
}
