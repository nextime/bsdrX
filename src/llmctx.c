/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
#include "bsdr/llmctx.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Each message is stored as its own heap string (the complete {...} object). Simpler and safer than
 * one packed buffer when compaction has to drop/insert in the middle. */
typedef struct { char *json; size_t len; int role; } msg;

struct bsdr_llmctx {
    msg   *m;
    int    n, cap;
};

bsdr_llmctx *bsdr_llmctx_new(void) {
    bsdr_llmctx *c = calloc(1, sizeof *c);
    return c;
}

void bsdr_llmctx_free(bsdr_llmctx *c) {
    if (!c) return;
    for (int i = 0; i < c->n; i++) free(c->m[i].json);
    free(c->m);
    free(c);
}

static bool grow(bsdr_llmctx *c) {
    if (c->n < c->cap) return true;
    int nc = c->cap ? c->cap * 2 : 16;
    msg *nm = realloc(c->m, (size_t)nc * sizeof *nm);
    if (!nm) return false;
    c->m = nm; c->cap = nc;
    return true;
}

bool bsdr_llmctx_push_n(bsdr_llmctx *c, int role, const char *json, size_t len) {
    if (!c || !json) return false;
    if (!grow(c)) return false;
    char *cp = malloc(len + 1);
    if (!cp) return false;
    memcpy(cp, json, len); cp[len] = '\0';
    c->m[c->n].json = cp; c->m[c->n].len = len; c->m[c->n].role = role;
    c->n++;
    return true;
}

bool bsdr_llmctx_push(bsdr_llmctx *c, int role, const char *json) {
    return bsdr_llmctx_push_n(c, role, json, json ? strlen(json) : 0);
}

int bsdr_llmctx_count(const bsdr_llmctx *c) { return c ? c->n : 0; }

size_t bsdr_llmctx_bytes(const bsdr_llmctx *c) {
    if (!c) return 0;
    size_t t = 0;
    for (int i = 0; i < c->n; i++) t += c->m[i].len + 1;   /* +1 for the joining comma */
    return t;
}

size_t bsdr_llmctx_est_tokens(const bsdr_llmctx *c) {
    return bsdr_llmctx_bytes(c) / 4;   /* ~4 bytes/token, deliberately rough */
}

size_t bsdr_llmctx_render(const bsdr_llmctx *c, int from, char *out, size_t cap) {
    if (!c || !out || cap == 0) { if (out && cap) out[0] = '\0'; return 0; }
    size_t o = 0;
    out[0] = '\0';
    if (from < 0) from = 0;
    for (int i = from; i < c->n; i++) {
        size_t need = c->m[i].len + (i > from ? 1 : 0);
        if (o + need + 1 > cap) { out[0] = '\0'; return 0; }   /* all-or-nothing: caller sizes for it */
        if (i > from) out[o++] = ',';
        memcpy(out + o, c->m[i].json, c->m[i].len);
        o += c->m[i].len;
    }
    out[o] = '\0';
    return o;
}

size_t bsdr_llmctx_render_range(const bsdr_llmctx *c, int first, int last, char *out, size_t cap) {
    if (!c || !out || cap == 0) { if (out && cap) out[0] = '\0'; return 0; }
    out[0] = '\0';
    if (first < 0) first = 0;
    if (last >= c->n) last = c->n - 1;
    if (first > last) return 0;
    size_t o = 0;
    for (int i = first; i <= last; i++) {
        size_t need = c->m[i].len + (i > first ? 1 : 0);
        if (o + need + 1 > cap) { out[0] = '\0'; return 0; }
        if (i > first) out[o++] = ',';
        memcpy(out + o, c->m[i].json, c->m[i].len);
        o += c->m[i].len;
    }
    out[o] = '\0';
    return o;
}

bool bsdr_llmctx_drop_range(const bsdr_llmctx *c, int keep_tail, int *first, int *last) {
    if (!c) return false;
    if (keep_tail < 0) keep_tail = 0;
    int head = 2;                       /* keep system + first user turn */
    if (c->n <= head) return false;
    int tail_start = c->n - keep_tail;
    if (tail_start < head) tail_start = head;
    if (tail_start <= head) return false;   /* nothing between head and tail to drop */
    if (first) *first = head;
    if (last)  *last  = tail_start - 1;
    return true;
}

/* Build a system message JSON wrapping `summary` (already plain text; we escape the essentials). */
static char *make_summary_msg(const char *summary) {
    /* minimal JSON escaping for a content string */
    size_t sl = strlen(summary);
    size_t cap = sl * 2 + 96;
    char *j = malloc(cap);
    if (!j) return NULL;
    size_t o = 0;
    const char *pre = "{\"role\":\"system\",\"content\":\"[earlier conversation summary] ";
    o += (size_t)snprintf(j + o, cap - o, "%s", pre);
    for (size_t i = 0; i < sl && o + 8 < cap; i++) {
        unsigned char ch = (unsigned char)summary[i];
        if (ch == '"' || ch == '\\') { j[o++] = '\\'; j[o++] = (char)ch; }
        else if (ch == '\n') { j[o++] = '\\'; j[o++] = 'n'; }
        else if (ch == '\r') { /* skip */ }
        else if (ch == '\t') { j[o++] = '\\'; j[o++] = 't'; }
        else if (ch < 0x20)  { j[o++] = ' '; }
        else j[o++] = (char)ch;
    }
    o += (size_t)snprintf(j + o, cap - o, "\"}");
    return j;
}

int bsdr_llmctx_compact(bsdr_llmctx *c, int strategy, int keep_tail, const char *summary) {
    if (!c) return 0;
    int first, last;
    if (!bsdr_llmctx_drop_range(c, keep_tail, &first, &last)) return c->n;

    /* Free the dropped messages. */
    for (int i = first; i <= last; i++) { free(c->m[i].json); c->m[i].json = NULL; }

    /* Optionally build a summary message to splice in at `first`. */
    char *summ = NULL;
    if ((strategy == BSDR_COMPACT_SUMMARY || strategy == BSDR_COMPACT_HYBRID) && summary && summary[0])
        summ = make_summary_msg(summary);

    /* Compose the new array: [0..first-1] + [summary?] + [last+1..n-1]. */
    int tail_n = c->n - (last + 1);
    int new_n = first + (summ ? 1 : 0) + tail_n;
    msg *nm = malloc((size_t)(new_n > 0 ? new_n : 1) * sizeof *nm);
    if (!nm) {
        /* On OOM, fall back to a plain truncate (no summary) which needs no allocation of msg array
         * beyond what we have — collapse in place. */
        free(summ);
        int w = first;
        for (int i = last + 1; i < c->n; i++) c->m[w++] = c->m[i];
        c->n = w;
        return c->n;
    }
    int w = 0;
    for (int i = 0; i < first; i++) nm[w++] = c->m[i];
    if (summ) { nm[w].json = summ; nm[w].len = strlen(summ); nm[w].role = BSDR_MSG_SYSTEM; w++; }
    for (int i = last + 1; i < c->n; i++) nm[w++] = c->m[i];
    free(c->m);
    c->m = nm; c->cap = new_n; c->n = w;
    return c->n;
}
