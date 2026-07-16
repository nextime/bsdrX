/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Tiered voice access-control — see bsdr/acl.h. */
#include "bsdr/acl.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

struct bsdr_acl {
    bsdr_mutex     *lock;
    bsdr_acl_entry  owner, bot;
    bsdr_acl_entry  friends[BSDR_ACL_MAX_FRIENDS];  int nfriends;
    bsdr_acl_entry  bans[BSDR_ACL_MAX_BANS];        int nbans;
    bool            friend_access;
    bool            host_access;
    uint32_t        group_mask;      /* which tool groups are globally enabled */
};

/* --- helpers (caller holds the lock) --------------------------------------- */

static bool entry_matches(const bsdr_acl_entry *e, const char *sid, const char *uname) {
    if (sid && sid[0] && e->social_id[0] && strcmp(sid, e->social_id) == 0) return true;
    if (uname && uname[0] && e->username[0] && strcasecmp(uname, e->username) == 0) return true;
    return false;
}

static void set_entry(bsdr_acl_entry *e, const char *sid, const char *uname) {
    snprintf(e->social_id, sizeof e->social_id, "%s", sid ? sid : "");
    snprintf(e->username,  sizeof e->username,  "%s", uname ? uname : "");
}

/* Append to a table if not already present (matched on either field). Returns false only when full.
 * If an existing entry matches but carries only one of the two identifiers, fill in the missing one. */
static bool table_add(bsdr_acl_entry *tab, int *n, int cap, const char *sid, const char *uname) {
    if ((!sid || !sid[0]) && (!uname || !uname[0])) return false;
    for (int i = 0; i < *n; i++) {
        if (entry_matches(&tab[i], sid, uname)) {
            if (sid && sid[0] && !tab[i].social_id[0]) snprintf(tab[i].social_id, sizeof tab[i].social_id, "%s", sid);
            if (uname && uname[0] && !tab[i].username[0]) snprintf(tab[i].username, sizeof tab[i].username, "%s", uname);
            return true;
        }
    }
    if (*n >= cap) return false;
    set_entry(&tab[(*n)++], sid, uname);
    return true;
}

static bool table_remove(bsdr_acl_entry *tab, int *n, const char *key) {
    for (int i = 0; i < *n; i++) {
        if (entry_matches(&tab[i], key, key)) {
            tab[i] = tab[--(*n)];   /* swap-with-last */
            return true;
        }
    }
    return false;
}

static bool table_has(const bsdr_acl_entry *tab, int n, const char *sid, const char *uname) {
    for (int i = 0; i < n; i++) if (entry_matches(&tab[i], sid, uname)) return true;
    return false;
}

/* --- lifecycle ------------------------------------------------------------- */

bsdr_acl *bsdr_acl_new(void) {
    bsdr_acl *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->lock = bsdr_mutex_new();
    a->friend_access = true;
    a->host_access   = true;
    a->group_mask    = BSDR_TG_ALL;
    return a;
}

void bsdr_acl_free(bsdr_acl *a) {
    if (!a) return;
    if (a->lock) bsdr_mutex_free(a->lock);
    free(a);
}

/* --- identity -------------------------------------------------------------- */

void bsdr_acl_set_owner(bsdr_acl *a, const char *sid, const char *uname) {
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    if (sid && sid[0])     snprintf(a->owner.social_id, sizeof a->owner.social_id, "%s", sid);
    if (uname && uname[0]) snprintf(a->owner.username,  sizeof a->owner.username,  "%s", uname);
    bsdr_mutex_unlock(a->lock);
}
size_t bsdr_acl_get_owner_social_id(bsdr_acl *a, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    size_t n = (size_t)snprintf(out, cap, "%s", a->owner.social_id);
    bsdr_mutex_unlock(a->lock);
    return n;
}
void bsdr_acl_set_bot(bsdr_acl *a, const char *sid, const char *uname) {
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    if (sid && sid[0])     snprintf(a->bot.social_id, sizeof a->bot.social_id, "%s", sid);
    if (uname && uname[0]) snprintf(a->bot.username,  sizeof a->bot.username,  "%s", uname);
    bsdr_mutex_unlock(a->lock);
}

/* --- friends / bans -------------------------------------------------------- */

bool bsdr_acl_friend_add(bsdr_acl *a, const char *sid, const char *uname) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool ok = table_add(a->friends, &a->nfriends, BSDR_ACL_MAX_FRIENDS, sid, uname);
    bsdr_mutex_unlock(a->lock);
    return ok;
}
bool bsdr_acl_friend_remove(bsdr_acl *a, const char *key) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool ok = table_remove(a->friends, &a->nfriends, key);
    bsdr_mutex_unlock(a->lock);
    return ok;
}
bool bsdr_acl_is_friend(bsdr_acl *a, const char *sid, const char *uname) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool ok = table_has(a->friends, a->nfriends, sid, uname);
    bsdr_mutex_unlock(a->lock);
    return ok;
}

bool bsdr_acl_ban_add(bsdr_acl *a, const char *sid, const char *uname) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool ok = table_add(a->bans, &a->nbans, BSDR_ACL_MAX_BANS, sid, uname);
    bsdr_mutex_unlock(a->lock);
    return ok;
}
bool bsdr_acl_ban_remove(bsdr_acl *a, const char *key) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool ok = table_remove(a->bans, &a->nbans, key);
    bsdr_mutex_unlock(a->lock);
    return ok;
}
bool bsdr_acl_is_banned(bsdr_acl *a, const char *sid, const char *uname) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool ok = table_has(a->bans, a->nbans, sid, uname);
    bsdr_mutex_unlock(a->lock);
    return ok;
}

int bsdr_acl_friends(bsdr_acl *a, bsdr_acl_entry *out, int cap) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    int n = a->nfriends < cap ? a->nfriends : cap;
    for (int i = 0; i < n; i++) out[i] = a->friends[i];
    int total = a->nfriends;
    bsdr_mutex_unlock(a->lock);
    return total;
}
int bsdr_acl_bans(bsdr_acl *a, bsdr_acl_entry *out, int cap) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    int n = a->nbans < cap ? a->nbans : cap;
    for (int i = 0; i < n; i++) out[i] = a->bans[i];
    int total = a->nbans;
    bsdr_mutex_unlock(a->lock);
    return total;
}

/* --- toggles --------------------------------------------------------------- */

void bsdr_acl_set_friend_access(bsdr_acl *a, bool on) { if (a) { bsdr_mutex_lock(a->lock); a->friend_access = on; bsdr_mutex_unlock(a->lock); } }
void bsdr_acl_set_host_access(bsdr_acl *a, bool on)   { if (a) { bsdr_mutex_lock(a->lock); a->host_access   = on; bsdr_mutex_unlock(a->lock); } }
bool bsdr_acl_friend_access(bsdr_acl *a) { if (!a) return false; bsdr_mutex_lock(a->lock); bool v = a->friend_access; bsdr_mutex_unlock(a->lock); return v; }
bool bsdr_acl_host_access(bsdr_acl *a)   { if (!a) return false; bsdr_mutex_lock(a->lock); bool v = a->host_access;   bsdr_mutex_unlock(a->lock); return v; }

void bsdr_acl_set_group_enabled(bsdr_acl *a, uint32_t group, bool on) {
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    if (on) a->group_mask |= group; else a->group_mask &= ~group;
    bsdr_mutex_unlock(a->lock);
}
bool bsdr_acl_group_enabled(bsdr_acl *a, uint32_t group) {
    if (!a) return false;
    bsdr_mutex_lock(a->lock);
    bool v = (a->group_mask & group) == group;
    bsdr_mutex_unlock(a->lock);
    return v;
}
uint32_t bsdr_acl_group_mask(bsdr_acl *a) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    uint32_t v = a->group_mask;
    bsdr_mutex_unlock(a->lock);
    return v;
}

/* --- resolution ------------------------------------------------------------ */

bsdr_acl_level bsdr_acl_resolve(bsdr_acl *a, const char *sid, const char *uname, bool is_room_host) {
    if (!a) return BSDR_ACL_NONE;
    bsdr_mutex_lock(a->lock);
    bsdr_acl_level lvl = BSDR_ACL_NONE;
    if (entry_matches(&a->owner, sid, uname)) {
        lvl = BSDR_ACL_OWNER;
    } else if (table_has(a->friends, a->nfriends, sid, uname)) {
        if (is_room_host && a->host_access)   lvl = BSDR_ACL_HOST;
        else if (a->friend_access)            lvl = BSDR_ACL_FRIEND;
        /* else NONE — access toggled off */
    }
    bsdr_mutex_unlock(a->lock);
    return lvl;
}

/* Base groups for a level before the global enable mask. Hierarchical: each level is a superset of
 * the one below. HOST bot-control is gated on own_room (they may only leave/restart the room they
 * host); the owner always gets bot-control everywhere. */
static uint32_t base_groups(bsdr_acl_level lvl, bool own_room) {
    switch (lvl) {
        case BSDR_ACL_OWNER:  return BSDR_TG_ALL;
        case BSDR_ACL_HOST:   return BSDR_TG_PUBLIC | BSDR_TG_MODERATOR | (own_room ? BSDR_TG_BOTCTL : 0u);
        case BSDR_ACL_FRIEND: return BSDR_TG_PUBLIC;
        default:              return 0u;
    }
}

uint32_t bsdr_acl_toolmask(bsdr_acl *a, bsdr_acl_level lvl, bool own_room) {
    if (!a) return 0;
    uint32_t base = base_groups(lvl, own_room);
    bsdr_mutex_lock(a->lock);
    uint32_t m = base & a->group_mask;
    bsdr_mutex_unlock(a->lock);
    return m;
}

const char *bsdr_acl_level_name(bsdr_acl_level lvl) {
    switch (lvl) {
        case BSDR_ACL_OWNER:  return "owner";
        case BSDR_ACL_HOST:   return "host";
        case BSDR_ACL_FRIEND: return "friend";
        default:              return "none";
    }
}

/* --- persistence (cJSON) --------------------------------------------------- */

static cJSON *entry_json(const bsdr_acl_entry *e) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "socialId", e->social_id);
    cJSON_AddStringToObject(o, "username", e->username);
    return o;
}

bool bsdr_acl_save(bsdr_acl *a, const char *path) {
    if (!a || !path) return false;
    bsdr_mutex_lock(a->lock);
    cJSON *root = cJSON_CreateObject();
    if (!root) { bsdr_mutex_unlock(a->lock); return false; }
    cJSON_AddItemToObject(root, "owner", entry_json(&a->owner));
    cJSON_AddItemToObject(root, "bot",   entry_json(&a->bot));
    cJSON_AddBoolToObject(root, "friendAccess", a->friend_access);
    cJSON_AddBoolToObject(root, "hostAccess",   a->host_access);
    cJSON_AddNumberToObject(root, "groupMask",  a->group_mask);
    cJSON *fr = cJSON_AddArrayToObject(root, "friends");
    for (int i = 0; i < a->nfriends; i++) cJSON_AddItemToArray(fr, entry_json(&a->friends[i]));
    cJSON *bn = cJSON_AddArrayToObject(root, "bans");
    for (int i = 0; i < a->nbans; i++) cJSON_AddItemToArray(bn, entry_json(&a->bans[i]));
    bsdr_mutex_unlock(a->lock);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) { ok = fwrite(txt, 1, strlen(txt), f) == strlen(txt); fclose(f); }
    else BSDR_WARN("bsdr.acl", "cannot write %s", path);
    free(txt);
    return ok;
}

static void load_entry(const cJSON *o, bsdr_acl_entry *e) {
    if (!cJSON_IsObject(o)) return;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "socialId");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(o, "username");
    set_entry(e, cJSON_IsString(s) ? s->valuestring : "", cJSON_IsString(u) ? u->valuestring : "");
}

bool bsdr_acl_load(bsdr_acl *a, const char *path) {
    if (!a || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;                       /* no file yet — keep defaults */
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 20) { fclose(f); return false; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { BSDR_WARN("bsdr.acl", "parse failed: %s", path); return false; }

    bsdr_mutex_lock(a->lock);
    load_entry(cJSON_GetObjectItemCaseSensitive(root, "owner"), &a->owner);
    load_entry(cJSON_GetObjectItemCaseSensitive(root, "bot"),   &a->bot);
    /* A stale "wakeWord" from an older access.json is ignored — it lives in the fullbot plugin now. */
    const cJSON *fa = cJSON_GetObjectItemCaseSensitive(root, "friendAccess");
    const cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "hostAccess");
    const cJSON *gm = cJSON_GetObjectItemCaseSensitive(root, "groupMask");
    if (cJSON_IsBool(fa)) a->friend_access = cJSON_IsTrue(fa);
    if (cJSON_IsBool(ha)) a->host_access   = cJSON_IsTrue(ha);
    if (cJSON_IsNumber(gm)) a->group_mask = (uint32_t)gm->valueint & BSDR_TG_ALL;
    a->nfriends = a->nbans = 0;
    const cJSON *arr, *it;
    arr = cJSON_GetObjectItemCaseSensitive(root, "friends");
    cJSON_ArrayForEach(it, arr) {
        if (a->nfriends >= BSDR_ACL_MAX_FRIENDS) break;
        load_entry(it, &a->friends[a->nfriends]);
        if (a->friends[a->nfriends].social_id[0] || a->friends[a->nfriends].username[0]) a->nfriends++;
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "bans");
    cJSON_ArrayForEach(it, arr) {
        if (a->nbans >= BSDR_ACL_MAX_BANS) break;
        load_entry(it, &a->bans[a->nbans]);
        if (a->bans[a->nbans].social_id[0] || a->bans[a->nbans].username[0]) a->nbans++;
    }
    int nf = a->nfriends, nb = a->nbans;
    bsdr_mutex_unlock(a->lock);
    cJSON_Delete(root);
    BSDR_INFO("bsdr.acl", "loaded %s: %d friends, %d bans", path, nf, nb);
    return true;
}
