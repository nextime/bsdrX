/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* Host-side tool registry — see include/bsdr/toolregistry.h and PLAN-bot-plugin.md §6.
 *
 * A small fixed table of tools plugins register into the bot's LLM loop. Enumeration and invocation
 * are filtered by the caller's permission group so a tool never reaches a caller not allowed to use it.
 * Guarded by an internal mutex; handlers are invoked AFTER dropping the lock (copy fn/owner under the
 * lock, then call), so a slow tool (shell_exec, screenshot, …) never blocks registration/enumeration. */
#include "bsdr/toolregistry.h"
#include "bsdr/platform.h"   /* bsdr_mutex */
#include "bsdr/json.h"       /* bsdr_json_escape */
#include "bsdr/log.h"

#include <string.h>
#include <stdio.h>

#define TAG "bsdr.tools"
#define TR_MAX      64      /* max simultaneously registered tools */
#define TR_NAME     64
#define TR_DESC     256
#define TR_SCHEMA   4096

typedef struct {
    int          used;
    char         name[TR_NAME];
    char         description[TR_DESC];
    char         schema[TR_SCHEMA];   /* JSON-schema of the parameters, or "" */
    unsigned     group;               /* required permission group (BSDR_TG_* bits); 0 = unrestricted */
    bsdr_tool_fn fn;
    void        *owner;               /* owner plugin's state pointer (unregister key) */
} tr_entry;

static tr_entry   g_tools[TR_MAX];
static int        g_count = 0;
static bsdr_mutex *g_lock = NULL;

/* Lazily created on first use. First registration happens during plugin init (load thread), matching
 * plugin.c's own lazy g_lock, so this is safe against the runtime readers that come later. */
static void tr_lock_init(void) { if (!g_lock) g_lock = bsdr_mutex_new(); }
static void tr_lock(void)   { if (g_lock) bsdr_mutex_lock(g_lock); }
static void tr_unlock(void) { if (g_lock) bsdr_mutex_unlock(g_lock); }

/* Find by name (caller holds the lock). Returns index or -1. */
static int tr_find(const char *name) {
    for (int i = 0; i < TR_MAX; i++)
        if (g_tools[i].used && strcmp(g_tools[i].name, name) == 0) return i;
    return -1;
}

int bsdr_tools_register(const char *name, const char *description, const char *schema_json,
                        unsigned group, bsdr_tool_fn fn, void *owner_state) {
    if (!name || !name[0] || strlen(name) >= TR_NAME || !fn) return 0;
    tr_lock_init();
    tr_lock();
    int i = tr_find(name);              /* replace in place if the name already exists */
    if (i < 0) {
        for (i = 0; i < TR_MAX; i++) if (!g_tools[i].used) break;
        if (i >= TR_MAX) { tr_unlock(); BSDR_WARN(TAG, "tool registry full (%d), '%s' rejected", TR_MAX, name); return 0; }
        g_tools[i].used = 1;
        g_count++;
    }
    snprintf(g_tools[i].name, sizeof g_tools[i].name, "%s", name);
    snprintf(g_tools[i].description, sizeof g_tools[i].description, "%s", description ? description : "");
    snprintf(g_tools[i].schema, sizeof g_tools[i].schema, "%s", schema_json ? schema_json : "");
    g_tools[i].group = group;
    g_tools[i].fn    = fn;
    g_tools[i].owner = owner_state;
    tr_unlock();
    BSDR_INFO(TAG, "registered tool '%s' (group=0x%x)", name, group);
    return 1;
}

void bsdr_tools_unregister(const char *name) {
    if (!name || !name[0]) return;
    tr_lock();
    int i = tr_find(name);
    if (i >= 0) { memset(&g_tools[i], 0, sizeof g_tools[i]); g_count--; }
    tr_unlock();
}

void bsdr_tools_unregister_owner(void *owner_state) {
    tr_lock();
    for (int i = 0; i < TR_MAX; i++)
        if (g_tools[i].used && g_tools[i].owner == owner_state) { memset(&g_tools[i], 0, sizeof g_tools[i]); g_count--; }
    tr_unlock();
}

/* A tool is visible to a caller iff every bit of its required group is within allowed_mask (group 0 =
 * always visible). */
static int tr_allowed(unsigned group, unsigned allowed_mask) {
    return (group & ~allowed_mask) == 0;
}

size_t bsdr_tools_list_json(unsigned allowed_mask, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    tr_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < TR_MAX && o < cap; i++) {
        if (!g_tools[i].used || !tr_allowed(g_tools[i].group, allowed_mask)) continue;
        char ne[TR_NAME * 2], de[TR_DESC * 2];
        bsdr_json_escape(ne, sizeof ne, g_tools[i].name);
        bsdr_json_escape(de, sizeof de, g_tools[i].description);
        /* schema is already JSON (an object or ""); emit {} when absent so parameters is always valid. */
        const char *schema = g_tools[i].schema[0] ? g_tools[i].schema : "{}";
        o += (size_t)snprintf(out + o, cap - o,
            "%s{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}",
            emitted ? "," : "", ne, de, schema);
        emitted++;
    }
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]");
    tr_unlock();
    return o;
}

int bsdr_tools_invoke(const char *name, const char *args_json, int caller_level,
                      unsigned caller_group_mask, char *out, size_t cap) {
    if (!name || !name[0]) return 0;
    /* Resolve under the lock, then release it BEFORE calling the handler so a slow tool doesn't block
     * the registry. The handler must not call back into the registry (documented contract). */
    tr_lock();
    int i = tr_find(name);
    if (i < 0) { tr_unlock(); return 0; }
    unsigned group = g_tools[i].group;
    bsdr_tool_fn fn = g_tools[i].fn;
    void *owner = g_tools[i].owner;
    tr_unlock();

    if (!tr_allowed(group, caller_group_mask)) {          /* defence in depth: re-check the caller */
        BSDR_WARN(TAG, "tool '%s' denied: caller mask 0x%x lacks group 0x%x", name, caller_group_mask, group);
        return 0;
    }
    if (out && cap) out[0] = '\0';
    return fn ? fn(owner, args_json ? args_json : "", caller_level, out, cap) : 0;
}

int bsdr_tools_count(void) {
    tr_lock();
    int n = g_count;
    tr_unlock();
    return n;
}
