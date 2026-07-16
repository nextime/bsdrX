/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Conversation context for the agentic LLM loop: an ordered list of chat messages that can be
 * rendered into a request body and, when it grows past a threshold, COMPACTED so a long multi-step
 * (e.g. coding) session fits the model's context window. Split out of llm.c so the buffer bookkeeping
 * and the three compaction strategies are unit-testable without a network. */
#ifndef BSDR_LLMCTX_H
#define BSDR_LLMCTX_H

#include <stddef.h>
#include <stdbool.h>

/* message roles (also decide what compaction may drop) */
enum { BSDR_MSG_SYSTEM = 0, BSDR_MSG_USER = 1, BSDR_MSG_ASSISTANT = 2, BSDR_MSG_TOOL = 3 };

/* compaction strategies */
enum {
    BSDR_COMPACT_TRUNCATE = 0,  /* drop the oldest middle messages, keep system+first user + tail */
    BSDR_COMPACT_SUMMARY  = 1,  /* replace the dropped middle with a single summary system message */
    BSDR_COMPACT_HYBRID   = 2,  /* summary of the older part + a larger verbatim recent tail */
};

typedef struct bsdr_llmctx bsdr_llmctx;

bsdr_llmctx *bsdr_llmctx_new(void);
void         bsdr_llmctx_free(bsdr_llmctx *c);

/* Append a message. `json` is the COMPLETE object, e.g. {"role":"user","content":"hi"}. Copied in.
 * Returns false on OOM. */
bool bsdr_llmctx_push(bsdr_llmctx *c, int role, const char *json);
bool bsdr_llmctx_push_n(bsdr_llmctx *c, int role, const char *json, size_t len);

int    bsdr_llmctx_count(const bsdr_llmctx *c);
size_t bsdr_llmctx_bytes(const bsdr_llmctx *c);   /* total rendered size (sum of message lengths) */

/* Rough token estimate for the whole conversation (≈ bytes / 4). Used only as a fallback when the
 * server doesn't report real usage. */
size_t bsdr_llmctx_est_tokens(const bsdr_llmctx *c);

/* Render messages [from .. count) joined by commas into `out` (no surrounding brackets). Returns the
 * number of bytes written (0 and out[0]='\0' if it doesn't fit). Pass from=0 for the full array. */
size_t bsdr_llmctx_render(const bsdr_llmctx *c, int from, char *out, size_t cap);

/* Render the inclusive message range [first..last] joined by commas (for summarizing a slice before
 * compaction). Returns bytes written (0 if it doesn't fit / bad range). */
size_t bsdr_llmctx_render_range(const bsdr_llmctx *c, int first, int last, char *out, size_t cap);

/* Compact the conversation in place. Always keeps message 0 (system) and message 1 (the first user
 * turn) and the last `keep_tail` messages; the middle is dropped. For SUMMARY/HYBRID, `summary`
 * (non-NULL) is inserted as a system message right after the first user turn describing the dropped
 * work. Returns the new message count, or the unchanged count if there was nothing to drop. */
int bsdr_llmctx_compact(bsdr_llmctx *c, int strategy, int keep_tail, const char *summary);

/* The byte range [out_from_off, out_to_off) that compaction WOULD drop, as message indices, so the
 * caller can render exactly that slice to build a summary before compacting. Fills first and last
 * with the inclusive message index range that would be removed (returns false if nothing to drop). */
bool bsdr_llmctx_drop_range(const bsdr_llmctx *c, int keep_tail, int *first, int *last);

#endif /* BSDR_LLMCTX_H */
