/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
/* LLM computer-control: tool calls -> uinput actions / app launch / bot+room control. */
#include "bsdr/compcontrol.h"
#include "bsdr/events.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/llm.h"      /* bsdr_llm_tool_group — the per-tool access guard */
#include "bsdr/app.h"      /* higher-tier tools reach the bot/room/acl through the app */
#include "bsdr/httpc.h"    /* web_read / web_search */
#include "bsdr/roomcmd.h"  /* translate_next arms the command router */
#include "bsdr/browserctl.h" /* browser_navigate / browser_eval via CDP */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

struct bsdr_compcontrol {
    bsdr_injector *inj;
    void (*speak)(void *user, const char *text);   /* optional TTS sink for the "speak" tool */
    void *speak_user;
    bsdr_app *app;                 /* enables the bot/room/admin/web tools (NULL = computer-only) */
    volatile int *abort;           /* the running command's abort flag (for stop_talking) */
    uint32_t       caller_mask;    /* tool groups the current caller may use */
    bsdr_acl_level caller_level;   /* the current caller's access level */
    char           caller_name[64];/* the current caller's display name (for admin/log) */
};

bsdr_compcontrol *bsdr_compcontrol_new(bsdr_injector *inj) {
    bsdr_compcontrol *cc = calloc(1, sizeof(*cc));
    if (cc) { cc->inj = inj; cc->caller_mask = BSDR_TG_ALL; cc->caller_level = BSDR_ACL_OWNER; }
    return cc;
}
void bsdr_compcontrol_set_speak(bsdr_compcontrol *cc, void (*cb)(void *, const char *), void *user) {
    if (!cc) return;
    cc->speak_user = user;   /* set user before cb so exec never sees a stale pair */
    cc->speak = cb;
}
void bsdr_compcontrol_set_app(bsdr_compcontrol *cc, bsdr_app *app) { if (cc) cc->app = app; }
void bsdr_compcontrol_set_abort(bsdr_compcontrol *cc, volatile int *abort) { if (cc) cc->abort = abort; }
void bsdr_compcontrol_set_caller(bsdr_compcontrol *cc, uint32_t mask, bsdr_acl_level level,
                                 const char *speaker) {
    if (!cc) return;
    cc->caller_mask = mask;
    cc->caller_level = level;
    snprintf(cc->caller_name, sizeof cc->caller_name, "%s", speaker ? speaker : "");
}
uint32_t bsdr_compcontrol_owner_mask(bsdr_compcontrol *cc) {
    if (cc && cc->app && cc->app->acl) {
        uint32_t m = bsdr_acl_toolmask(cc->app->acl, BSDR_ACL_OWNER, true);
        if (cc->app->browser_ctl_enabled && cc->app->cdp_endpoint[0]) m |= BSDR_TG_BROWSER;   /* opt-in */
        return m;
    }
    return BSDR_TG_ALL;
}
void bsdr_compcontrol_free(bsdr_compcontrol *cc) { free(cc); }

static void key_event(bsdr_injector *inj, uint16_t vk, bool down) {
    bsdr_input_event e;
    e.kind = BSDR_EV_KEY; e.u.key.is_vk = true; e.u.key.value = vk; e.u.key.down = down;
    bsdr_injector_handle(inj, &e);
}
static void char_event(bsdr_injector *inj, uint16_t ch, bool down) {
    bsdr_input_event e;
    e.kind = BSDR_EV_KEY; e.u.key.is_vk = false; e.u.key.value = ch; e.u.key.down = down;
    bsdr_injector_handle(inj, &e);
}

/* Windows VK codes (the injector maps VK -> native). */
static uint16_t name_to_vk(const char *n) {
    if (strlen(n) == 1) {
        char c = (char)toupper((unsigned char)n[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (uint16_t)c;
    }
    if (!strcasecmp(n, "ctrl") || !strcasecmp(n, "control")) return 0x11;
    if (!strcasecmp(n, "alt")) return 0x12;
    if (!strcasecmp(n, "shift")) return 0x10;
    if (!strcasecmp(n, "super") || !strcasecmp(n, "meta") || !strcasecmp(n, "win")) return 0x5B;
    if (!strcasecmp(n, "enter") || !strcasecmp(n, "return")) return 0x0D;
    if (!strcasecmp(n, "tab")) return 0x09;
    if (!strcasecmp(n, "esc") || !strcasecmp(n, "escape")) return 0x1B;
    if (!strcasecmp(n, "space")) return 0x20;
    if (!strcasecmp(n, "backspace")) return 0x08;
    if (!strcasecmp(n, "delete") || !strcasecmp(n, "del")) return 0x2E;
    if (!strcasecmp(n, "up")) return 0x26;
    if (!strcasecmp(n, "down")) return 0x28;
    if (!strcasecmp(n, "left")) return 0x25;
    if (!strcasecmp(n, "right")) return 0x27;
    if (!strcasecmp(n, "home")) return 0x24;
    if (!strcasecmp(n, "end")) return 0x23;
    return 0;
}

static void do_type(bsdr_injector *inj, const char *text) {
    for (const char *p = text; *p; p++) {
        char_event(inj, (uint8_t)*p, true);
        char_event(inj, (uint8_t)*p, false);
    }
}

/* "ctrl+shift+t" -> modifiers held around the final key */
static void do_key(bsdr_injector *inj, const char *combo) {
    char buf[128]; snprintf(buf, sizeof(buf), "%s", combo);
    uint16_t mods[4]; int nm = 0; uint16_t key = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        while (*tok == ' ') tok++;
        uint16_t vk = name_to_vk(tok);
        if (vk == 0x11 || vk == 0x12 || vk == 0x10 || vk == 0x5B) {
            if (nm < 4) mods[nm++] = vk;
        } else {
            key = vk;
        }
    }
    for (int i = 0; i < nm; i++) key_event(inj, mods[i], true);
    if (key) { key_event(inj, key, true); key_event(inj, key, false); }
    for (int i = nm - 1; i >= 0; i--) key_event(inj, mods[i], false);
}

static void do_click(bsdr_injector *inj, double x, double y, const char *button) {
    bsdr_input_event mv;
    mv.kind = BSDR_EV_MOVE_ABS; mv.u.move_abs.x = x; mv.u.move_abs.y = y;
    bsdr_injector_handle(inj, &mv);
    bsdr_mouse_button b = BSDR_BTN_LEFT;
    if (button && !strcasecmp(button, "right")) b = BSDR_BTN_RIGHT;
    else if (button && !strcasecmp(button, "middle")) b = BSDR_BTN_MIDDLE;
    bsdr_input_event d; d.kind = BSDR_EV_BUTTON; d.u.button.button = b; d.u.button.down = true;
    bsdr_injector_handle(inj, &d);
    d.u.button.down = false; bsdr_injector_handle(inj, &d);
}

static void do_scroll(bsdr_injector *inj, int amount) {
    bsdr_input_event e; e.kind = BSDR_EV_SCROLL; e.u.scroll.dx = 0; e.u.scroll.dy = amount;
    bsdr_injector_handle(inj, &e);
}

/* Public actuators (thin, so the existing in-file callers stay unchanged) for the host input services. */
void bsdr_cc_type(bsdr_injector *inj, const char *text)  { if (inj && text) do_type(inj, text); }
void bsdr_cc_key(bsdr_injector *inj, const char *combo)  { if (inj && combo) do_key(inj, combo); }
void bsdr_cc_click(bsdr_injector *inj, double x, double y, const char *button) { if (inj) do_click(inj, x, y, button); }
void bsdr_cc_scroll(bsdr_injector *inj, int amount)      { if (inj) do_scroll(inj, amount); }

static void do_open(const char *name, char *result, size_t rl) {
    /* allow only a plain command word + simple args; launch detached */
    for (const char *p = name; *p; p++)
        if (strchr(";|&`$<>(){}\n", *p)) { snprintf(result, rl, "rejected"); return; }
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "setsid %s >/dev/null 2>&1 &", name);
    int rc = system(cmd);
    snprintf(result, rl, rc == 0 ? "launched" : "launch failed");
}

/* --- owner computer tools: shell + files ---------------------------------- */

static void do_shell(const char *cmd, char *result, size_t rl) {
#if defined(_WIN32) || defined(BSDR_PLATFORM_ANDROID)
    (void)cmd; snprintf(result, rl, "shell not available on this platform");
#else
    FILE *p = popen(cmd, "r");
    if (!p) { snprintf(result, rl, "failed to run"); return; }
    size_t o = 0;
    int c;
    while (o + 1 < rl && (c = fgetc(p)) != EOF) result[o++] = (char)c;
    result[o] = '\0';
    int rc = pclose(p);
    if (o == 0) snprintf(result, rl, "(no output, exit %d)", rc);
#endif
}

static void do_read_file(const char *path, char *result, size_t rl) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(result, rl, "cannot open %s", path); return; }
    size_t n = fread(result, 1, rl - 1, f);
    result[n] = '\0';
    fclose(f);
    if (n == 0) snprintf(result, rl, "(empty file)");
}

static void do_write_file(const char *path, const char *content, char *result, size_t rl) {
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(result, rl, "cannot write %s", path); return; }
    size_t len = content ? strlen(content) : 0;
    size_t w = fwrite(content, 1, len, f);
    fclose(f);
    snprintf(result, rl, w == len ? "wrote %zu bytes" : "partial write", w);
}

/* --- public web tools ------------------------------------------------------ */

/* Strip HTML tags + collapse whitespace from `in` into `out` (truncated). Good enough to hand a page's
 * readable text to the model. */
static void html_to_text(const char *in, char *out, size_t cap) {
    size_t o = 0; int intag = 0, sp = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        if (*p == '<') { intag = 1; continue; }
        if (*p == '>') { intag = 0; continue; }
        if (intag) continue;
        char c = *p;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { if (!sp && o) { out[o++] = ' '; sp = 1; } }
        else { out[o++] = c; sp = 0; }
    }
    out[o] = '\0';
}

static void do_web_read(const char *url, char *result, size_t rl) {
    if (!url || !url[0]) { snprintf(result, rl, "no url"); return; }
    size_t cap = 64 * 1024;
    char *resp = malloc(cap);
    if (!resp) { snprintf(result, rl, "oom"); return; }
    int r = bsdr_http_request("GET", url, NULL, 0, NULL, NULL, 0, resp, cap);
    if (r < 0 || bsdr_http_status(resp) / 100 != 2) { snprintf(result, rl, "fetch failed"); free(resp); return; }
    const char *body = bsdr_http_body(resp);
    html_to_text(body ? body : "", result, rl);
    free(resp);
}

static void do_web_search(bsdr_app *app, const char *query, char *result, size_t rl) {
    char ep[256] = "", tok[256] = "";
    if (app) {
        bsdr_mutex_lock(app->lock);
        snprintf(ep, sizeof ep, "%s", app->web_search_endpoint);
        snprintf(tok, sizeof tok, "%s", app->web_search_token);
        bsdr_mutex_unlock(app->lock);
    }
    if (!ep[0]) { snprintf(result, rl, "web search not configured (the owner can set a search endpoint)"); return; }
    char url[600]; char q[256];
    /* minimal URL-encode of the query (space -> %20, keep it simple) */
    size_t qo = 0;
    for (const char *p = query; p && *p && qo + 4 < sizeof q; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) q[qo++] = (char)c;
        else { static const char hx[] = "0123456789ABCDEF"; q[qo++] = '%'; q[qo++] = hx[c >> 4]; q[qo++] = hx[c & 15]; }
    }
    q[qo] = '\0';
    snprintf(url, sizeof url, "%s%s%s", ep, strchr(ep, '?') ? "&q=" : "?q=", q);
    size_t cap = 32 * 1024; char *resp = malloc(cap);
    if (!resp) { snprintf(result, rl, "oom"); return; }
    bsdr_http_header h[1]; int nh = 0; char auth[300];
    if (tok[0]) { snprintf(auth, sizeof auth, "Bearer %s", tok); h[0].name = "Authorization"; h[0].value = auth; nh = 1; }
    int r = bsdr_http_request("GET", url, h, nh, NULL, NULL, 0, resp, cap);
    if (r < 0 || bsdr_http_status(resp) / 100 != 2) { snprintf(result, rl, "search failed"); free(resp); return; }
    const char *body = bsdr_http_body(resp);
    snprintf(result, rl, "%s", body ? body : "(no results)");
    free(resp);
}

/* Resolve a target username to a friend add/remove etc.; the app's roster socialId if we have it. */
static const char *roster_socialid(bsdr_app *app, const char *username) {
    if (!app || !username || !username[0]) return NULL;
    const bsdr_roster_entry *e = bsdr_roster_by_username(&app->roster, username);
    return (e && e->social_id[0]) ? e->social_id : NULL;
}

void bsdr_compcontrol_exec(const char *name, const char *args, char *result,
                           size_t rl, void *user) {
    bsdr_compcontrol *cc = (bsdr_compcontrol *)user;
    snprintf(result, rl, "ok");
    bsdr_app *app = cc->app;

    /* Access guard (defense-in-depth): a known tool outside the caller's mask is refused even if the
     * model somehow calls it. Unknown tools fall through to the dispatch's "unknown tool". */
    uint32_t grp = bsdr_llm_tool_group(name);
    if (grp && !(grp & cc->caller_mask)) {
        BSDR_WARN("bsdr.cc", "tool %s denied for %s (level %s)", name,
                  cc->caller_name[0] ? cc->caller_name : "?", bsdr_acl_level_name(cc->caller_level));
        snprintf(result, rl, "not permitted for your access level");
        return;
    }

    if (!strcmp(name, "type_text")) {
        char text[2048] = "";
        bsdr_json_get_str(args, "text", text, sizeof(text));
        do_type(cc->inj, text);
    } else if (!strcmp(name, "key")) {
        char keys[128] = "";
        bsdr_json_get_str(args, "keys", keys, sizeof(keys));
        do_key(cc->inj, keys);
    } else if (!strcmp(name, "click")) {
        double x = 0.5, y = 0.5; char btn[16] = "left";
        bsdr_json_get_double(args, "x", &x);
        bsdr_json_get_double(args, "y", &y);
        bsdr_json_get_str(args, "button", btn, sizeof(btn));
        do_click(cc->inj, x, y, btn);
    } else if (!strcmp(name, "scroll")) {
        double a = 0; bsdr_json_get_double(args, "amount", &a);
        do_scroll(cc->inj, (int)a);
    } else if (!strcmp(name, "open_app")) {
        char appn[128] = "";
        bsdr_json_get_str(args, "name", appn, sizeof(appn));
        do_open(appn, result, rl);
    } else if (!strcmp(name, "shell_exec")) {
        char cmd[1024] = "";
        bsdr_json_get_str(args, "command", cmd, sizeof(cmd));
        if (cmd[0]) do_shell(cmd, result, rl); else snprintf(result, rl, "no command");
    } else if (!strcmp(name, "read_file")) {
        char path[512] = "";
        bsdr_json_get_str(args, "path", path, sizeof(path));
        if (path[0]) do_read_file(path, result, rl); else snprintf(result, rl, "no path");
    } else if (!strcmp(name, "write_file")) {
        char path[512] = "", *content = malloc(8192);
        if (!content) { snprintf(result, rl, "oom"); return; }
        content[0] = '\0';
        bsdr_json_get_str(args, "path", path, sizeof(path));
        bsdr_json_get_str(args, "content", content, 8192);
        if (path[0]) do_write_file(path, content, result, rl); else snprintf(result, rl, "no path");
        free(content);
    } else if (!strcmp(name, "speak")) {
        char text[2048] = "";
        bsdr_json_get_str(args, "text", text, sizeof(text));
        if (cc->speak && text[0]) { cc->speak(cc->speak_user, text); snprintf(result, rl, "spoke"); }
        else snprintf(result, rl, cc->speak ? "nothing to say" : "speech not configured");
    } else if (!strcmp(name, "web_read")) {
        char url[600] = "";
        bsdr_json_get_str(args, "url", url, sizeof(url));
        do_web_read(url, result, rl);
    } else if (!strcmp(name, "web_search")) {
        char q[256] = "";
        bsdr_json_get_str(args, "query", q, sizeof(q));
        do_web_search(app, q, result, rl);
    } else if (!strcmp(name, "browser_navigate") || !strcmp(name, "browser_eval")) {
        char ep[256] = ""; bool on = false;
        if (app) { bsdr_mutex_lock(app->lock); on = app->browser_ctl_enabled;
                   snprintf(ep, sizeof ep, "%s", app->cdp_endpoint); bsdr_mutex_unlock(app->lock); }
        if (!on || !ep[0]) { snprintf(result, rl, "browser control is disabled (enable it and set a CDP endpoint in the web panel)"); }
        else if (!strcmp(name, "browser_navigate")) {
            char url[1024] = ""; bsdr_json_get_str(args, "url", url, sizeof url);
            bsdr_browser_navigate(ep, url, result, rl);
        } else {
            char expr[3000] = ""; bsdr_json_get_str(args, "expression", expr, sizeof expr);
            bsdr_browser_eval(ep, expr, result, rl);
        }
    } else if (!strcmp(name, "reset_room")) {
        if (!app) snprintf(result, rl, "reset unavailable");
        else { int n = bsdr_app_reset_room(app);
               snprintf(result, rl, "reset the room — removed %d participant(s); everyone can rejoin fresh", n); }
    } else if (!strcmp(name, "stop_talking")) {
        if (cc->abort) *cc->abort = 1;
        snprintf(result, rl, "stopping");
    } else if (!strcmp(name, "kick_user")) {
        char who[64] = ""; bsdr_json_get_str(args, "username", who, sizeof who);
        if (!app) snprintf(result, rl, "moderation unavailable");
        else snprintf(result, rl, bsdr_app_kick_user(app, who) ? "kicked %s" : "could not kick %s", who);
    } else if (!strcmp(name, "ban_user")) {
        char who[64] = ""; bsdr_json_get_str(args, "username", who, sizeof who);
        if (!app) snprintf(result, rl, "moderation unavailable");
        else { bool k = bsdr_app_ban_user(app, who); snprintf(result, rl, k ? "banned %s" : "banned %s (rejoin will be blocked)", who); }
    } else if (!strcmp(name, "mic_check")) {
        char who[64] = ""; bsdr_json_get_str(args, "username", who, sizeof who);
        if (!app || !app->roomcmd) { snprintf(result, rl, "mic-check unavailable (bot not in a room)"); }
        else {
            const bsdr_roster_entry *e = bsdr_roster_by_username(&app->roster, who);
            if (!e) snprintf(result, rl, "no one named %s is in the room", who);
            else {
                bsdr_mutex_lock(app->lock); app->mic_check_ssrc = e->ssrc; bsdr_mutex_unlock(app->lock);
                bsdr_roomcmd_arm_miccheck((bsdr_roomcmd *)app->roomcmd, e->ssrc, who);
                snprintf(result, rl, "age-checking %s", who);
            }
        }
    } else if (!strcmp(name, "mic_check_enable")) {
        double on = 1; bsdr_json_get_double(args, "on", &on);
        if (app) { bsdr_mutex_lock(app->lock); app->mic_check_auto = on != 0; bsdr_mutex_unlock(app->lock);
                   bsdr_app_save_settings(app); snprintf(result, rl, on ? "auto mic-check on" : "auto mic-check off"); }
        else snprintf(result, rl, "unavailable");
    } else if (!strcmp(name, "translate_next")) {
        char who[64] = "", lang[32] = "";
        bsdr_json_get_str(args, "username", who, sizeof who);
        bsdr_json_get_str(args, "to_language", lang, sizeof lang);
        if (!app || !app->roomcmd) { snprintf(result, rl, "translation unavailable (bot not in a room)"); }
        else {
            const bsdr_roster_entry *e = bsdr_roster_by_username(&app->roster, who);
            if (!e) snprintf(result, rl, "no one named %s is in the room", who);
            else {
                bsdr_mutex_lock(app->lock); app->mic_check_ssrc = e->ssrc; bsdr_mutex_unlock(app->lock);
                bsdr_roomcmd_arm_translate((bsdr_roomcmd *)app->roomcmd, e->ssrc, lang[0] ? lang : "English");
                snprintf(result, rl, "listening — will translate %s's next words into %s", who, lang[0] ? lang : "English");
            }
        }
    } else if (!strcmp(name, "follow_me")) {
        double on = 1; bsdr_json_get_double(args, "on", &on);
        if (app) { bsdr_app_set_bot_follow(app, on != 0); snprintf(result, rl, on ? "following you" : "no longer following"); }
        else snprintf(result, rl, "bot control unavailable");
    } else if (!strcmp(name, "leave_room")) {
        if (app) { bsdr_app_set_bot_follow(app, false); bsdr_app_bot_leave_room(app); snprintf(result, rl, "leaving the room"); }
        else snprintf(result, rl, "bot control unavailable");
    } else if (!strcmp(name, "restart_bot")) {
        if (app) { bsdr_app_bot_leave_room(app); bsdr_app_bot_join_room(app); snprintf(result, rl, "rejoining"); }
        else snprintf(result, rl, "bot control unavailable");
    } else if (!strcmp(name, "stay_with")) {
        if (cc->caller_level < BSDR_ACL_OWNER) { snprintf(result, rl, "only the owner can do that"); }
        else if (app) {
            char who[64] = ""; bsdr_json_get_str(args, "username", who, sizeof who);
            bsdr_mutex_lock(app->lock);
            snprintf(app->stay_with, sizeof app->stay_with, "%s", who);
            bsdr_mutex_unlock(app->lock);
            bsdr_app_set_bot_follow(app, false);   /* stay-with overrides follow-me */
            snprintf(result, rl, who[0] ? "will stay in %s's room" : "stay-with cleared", who);
        } else snprintf(result, rl, "bot control unavailable");
    } else if (!strcmp(name, "authorize") || !strcmp(name, "deauthorize")) {
        char what[32] = "", who[64] = "";
        bsdr_json_get_str(args, "what", what, sizeof what);
        bsdr_json_get_str(args, "username", who, sizeof who);
        bool add = !strcmp(name, "authorize");
        if (!app || !app->acl) { snprintf(result, rl, "unavailable"); }
        else if (!strcasecmp(what, "friend")) {
            if (add) bsdr_acl_friend_add(app->acl, roster_socialid(app, who), who);
            else     bsdr_acl_friend_remove(app->acl, who);
            bsdr_app_acl_save(app);
            snprintf(result, rl, add ? "added %s as a friend" : "removed friend %s", who);
        } else snprintf(result, rl, "unknown authorization target '%s'", what);
    } else if (!strcmp(name, "set_access")) {
        char which[32] = ""; double en = 1;
        bsdr_json_get_str(args, "which", which, sizeof which);
        bsdr_json_get_double(args, "enabled", &en);
        bool on = en != 0;
        if (!app || !app->acl) { snprintf(result, rl, "unavailable"); }
        else if (!strcasecmp(which, "friends")) { bsdr_acl_set_friend_access(app->acl, on); goto acl_ok; }
        else if (!strcasecmp(which, "hosts"))   { bsdr_acl_set_host_access(app->acl, on);   goto acl_ok; }
        else {
            uint32_t g = !strcasecmp(which,"public")?BSDR_TG_PUBLIC : !strcasecmp(which,"moderator")?BSDR_TG_MODERATOR
                       : !strcasecmp(which,"botctl")?BSDR_TG_BOTCTL : !strcasecmp(which,"computer")?BSDR_TG_COMPUTER
                       : !strcasecmp(which,"admin")?BSDR_TG_ADMIN : 0;
            if (!g) { snprintf(result, rl, "unknown access '%s'", which); }
            else { bsdr_acl_set_group_enabled(app->acl, g, on);
                   acl_ok: bsdr_app_acl_save(app); snprintf(result, rl, "%s %s", which, on ? "enabled" : "disabled"); }
        }
    } else {
        snprintf(result, rl, "unknown tool");
    }
    BSDR_INFO("bsdr.cc", "exec %s -> %s", name, result);
}
