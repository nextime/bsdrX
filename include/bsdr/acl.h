/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Tiered voice access-control for the in-room bot.
 *
 * Every room speaker resolves to a hierarchical LEVEL — Owner ▸ Host ▸ Friend ▸ none — and each
 * level is granted a set of TOOL GROUPS (a bitmask). A higher level inherits every tool of the
 * lower levels. The same resolution drives both the per-speaker audio-volume policy and the set of
 * tools the LLM is offered for that speaker's command.
 *
 *   Owner   = the primary bsdrX account (the desktop companion). Top level, everything.
 *   Host    = a friend who is hosting the room the bot is in (auto-promoted; toggleable).
 *   Friend  = one of the bot account's approved friends (toggleable).
 *   none    = everyone else — silenced, no tools.
 *
 * Thread-safe: the resolver is read from the audio-policy + command-router threads while the web UI
 * / admin tools mutate the friend/ban lists and toggles. All state sits behind an internal mutex.
 * Persisted to access.json (cJSON) so a headless bot keeps its friends/bans/toggles across restarts. */
#ifndef BSDR_ACL_H
#define BSDR_ACL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hierarchical access levels (higher inherits all lower tools). */
typedef enum {
    BSDR_ACL_NONE   = 0,
    BSDR_ACL_FRIEND = 1,
    BSDR_ACL_HOST   = 2,
    BSDR_ACL_OWNER  = 3,
} bsdr_acl_level;

/* Tool groups (bitmask). Each group also has a global owner-controlled enable bit. */
enum {
    BSDR_TG_PUBLIC    = 1u << 0,   /* web_search, web_read, chat, stop_talking */
    BSDR_TG_MODERATOR = 1u << 1,   /* kick, ban, mic_check, mic_check_enable */
    BSDR_TG_BOTCTL    = 1u << 2,   /* follow_me, leave, stop, restart, stay_with */
    BSDR_TG_COMPUTER  = 1u << 3,   /* shell/files/vision/input/open_app */
    BSDR_TG_ADMIN     = 1u << 4,   /* authorize/deauthorize user·host·friend·command */
    BSDR_TG_BROWSER   = 1u << 5,   /* browser_navigate/browser_eval via CDP — owner-only, opt-in */
};
/* NB: BROWSER is deliberately NOT in ALL — it's owner-only AND off unless the operator enables it and
 * configures a CDP endpoint; the caller ORs it into the mask only then. */
#define BSDR_TG_ALL (BSDR_TG_PUBLIC|BSDR_TG_MODERATOR|BSDR_TG_BOTCTL|BSDR_TG_COMPUTER|BSDR_TG_ADMIN)

#define BSDR_ACL_MAX_FRIENDS 128
#define BSDR_ACL_MAX_BANS    128

/* A social identity. socialId is the stable key; username is what the room roster exposes, so a
 * speaker is matched on EITHER (socialId exact, username case-insensitive). */
typedef struct {
    char social_id[80];
    char username[64];
} bsdr_acl_entry;

typedef struct bsdr_acl bsdr_acl;

bsdr_acl *bsdr_acl_new(void);
void      bsdr_acl_free(bsdr_acl *a);

/* Identity of the two accounts. owner = the primary bsdrX account (top level). Either field may be
 * empty if not yet known; matching then falls back to whichever is set. */
void bsdr_acl_set_owner(bsdr_acl *a, const char *owner_social_id, const char *owner_username);
/* Copy the cached owner socialId into `out` (empty if not resolved yet). Lets the startup path skip a
 * redundant network resolve when access.json already carries it. Returns the length. */
size_t bsdr_acl_get_owner_social_id(bsdr_acl *a, char *out, size_t cap);
void bsdr_acl_set_bot(bsdr_acl *a, const char *bot_social_id, const char *bot_username);

/* No wake word here. It only means something when the fullbot plugin is loaded — the core bot never
 * answers to its name in the room; it just runs the owner's balloon commands. The one wake word is
 * the fullbot plugin's ("wake" in its config). */

/* Friends (Friend level). Idempotent. Returns false only on a full table. */
bool bsdr_acl_friend_add(bsdr_acl *a, const char *social_id, const char *username);
bool bsdr_acl_friend_remove(bsdr_acl *a, const char *social_id_or_username);
bool bsdr_acl_is_friend(bsdr_acl *a, const char *social_id, const char *username);

/* Soft-ban list (kick + persist + auto-re-kick on rejoin — the caller does the kicking). */
bool bsdr_acl_ban_add(bsdr_acl *a, const char *social_id, const char *username);
bool bsdr_acl_ban_remove(bsdr_acl *a, const char *social_id_or_username);
bool bsdr_acl_is_banned(bsdr_acl *a, const char *social_id, const char *username);

/* Copy the friend / ban tables out (for the web UI). Returns the count; fills up to `cap`. */
int bsdr_acl_friends(bsdr_acl *a, bsdr_acl_entry *out, int cap);
int bsdr_acl_bans(bsdr_acl *a, bsdr_acl_entry *out, int cap);

/* Master toggles + per-group global enable. */
void bsdr_acl_set_friend_access(bsdr_acl *a, bool on);
void bsdr_acl_set_host_access(bsdr_acl *a, bool on);
bool bsdr_acl_friend_access(bsdr_acl *a);
bool bsdr_acl_host_access(bsdr_acl *a);
void bsdr_acl_set_group_enabled(bsdr_acl *a, uint32_t group, bool on);
bool bsdr_acl_group_enabled(bsdr_acl *a, uint32_t group);
uint32_t bsdr_acl_group_mask(bsdr_acl *a);

/* Resolve a speaker to a level. is_room_host = this speaker owns/hosts the room the bot is in. */
bsdr_acl_level bsdr_acl_resolve(bsdr_acl *a, const char *social_id, const char *username,
                                bool is_room_host);

/* Tool groups granted to `lvl`. own_room = the bot is acting in a room this speaker hosts (or the
 * owner's own room / a stay-with target) — gates HOST bot-control. AND-ed with the global mask. */
uint32_t bsdr_acl_toolmask(bsdr_acl *a, bsdr_acl_level lvl, bool own_room);

/* Persist to / load from a JSON file. load() tolerates a missing file (returns false, leaves
 * defaults). Both are NULL-safe. */
bool bsdr_acl_save(bsdr_acl *a, const char *path);
bool bsdr_acl_load(bsdr_acl *a, const char *path);

const char *bsdr_acl_level_name(bsdr_acl_level lvl);

#endif /* BSDR_ACL_H */
