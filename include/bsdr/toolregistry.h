/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* Host-side tool registry (ABI 4 bot host-service surface — see PLAN-bot-plugin.md §6).
 *
 * Plugins register named tools (with a required permission group + JSON-schema + handler); the bot
 * plugin (fullbot) enumerates the tools a given caller may use and dispatches invocations back to the
 * owning plugin. This lets the bot's moderation / translation / computer-control plugins inject tools
 * into fullbot's LLM loop WITHOUT any plugin->plugin ABI — everything is mediated by the host, which
 * keeps the registry. Exposed to plugins as host->tool_register / _unregister / _list_json / _invoke.
 *
 * Thread-safe: guarded by an internal mutex, so a registration (a plugin's init/load thread) can't race
 * an enumeration/invocation (the bot's command-loop thread). */
#ifndef BSDR_TOOLREGISTRY_H
#define BSDR_TOOLREGISTRY_H

#include <stddef.h>
#include "bsdr/plugin.h"   /* bsdr_tool_fn */

/* Register or replace a tool. `group` is the minimum permission group required to call it (use the
 * bot's BSDR_TG_* bits). `schema_json` is the JSON-schema of the parameters (may be "" for none).
 * `owner_state` is passed back to fn and used as the key for bsdr_tools_unregister_owner. Re-registering
 * an existing name replaces it (last writer wins). Returns 1 on success, 0 if the table is full or the
 * arguments are invalid. */
int  bsdr_tools_register(const char *name, const char *description, const char *schema_json,
                         unsigned group, bsdr_tool_fn fn, void *owner_state);

/* Remove one tool by name (no-op if absent). */
void bsdr_tools_unregister(const char *name);

/* Remove every tool registered with this owner_state — called by the plugin manager when a plugin is
 * unloaded/reloaded, so a plugin's tools never outlive it. */
void bsdr_tools_unregister_owner(void *owner_state);

/* Append the model-facing tool-list as a JSON array — [{"name","description","parameters":<schema>},…]
 * — for every tool whose group is fully within `allowed_mask`. Returns bytes written (an empty array
 * "[]" if none). Used by fullbot to assemble a caller's toolset from its level mask. */
size_t bsdr_tools_list_json(unsigned allowed_mask, char *out, size_t cap);

/* Invoke a registered tool by name. Before dispatching, re-checks that `caller_group_mask` contains the
 * tool's required group (defence in depth: a tool can't run for a caller whose level doesn't include
 * it, even if the caller-side filtering was wrong). Returns 1 and fills out (a JSON result string) on
 * success; 0 if the tool is unknown or the caller isn't permitted. */
int  bsdr_tools_invoke(const char *name, const char *args_json, int caller_level,
                       unsigned caller_group_mask, char *out, size_t cap);

/* Number of currently registered tools (status / tests). */
int  bsdr_tools_count(void);

#endif /* BSDR_TOOLREGISTRY_H */
