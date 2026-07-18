/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* Host-side loadable-plugin manager — see bsdr/plugin.h for the ABI. */
#include "bsdr/plugin.h"
#include "bsdr/toolregistry.h" /* bot host-service: tool registry exposed via the host table (ABI 4) */
#include "bsdr/mediafx.h"    /* media-effect registry (voice-changer / 2D->3D) */
#include "bsdr/model_store.h" /* shared model cache exposed to model-using plugins */
#include "bsdr/voicestore.h"  /* RVC voice library exposed to the voice-changer plugin */
#include "bsdr/plugstore.h" /* bsdr_plugin_name_disabled — skip operator-disabled plugins */
#include "bsdr/platform.h"  /* bsdr_mutex — guard the plugin list against live reload vs readers */
#include "bsdr/log.h"
#include "bsdr/json.h"
#include "bsdr/webui.h"   /* bsdr_webui_plugin_respond — host http reply used by the host table */
#include "bsdr/app.h"     /* bsdr_config_dir + bot host-service accessors (roster/kick/…) */
#include "bsdr/screenshot.h" /* bsdr_screenshot_jpeg — desktop capture host service */
#include "bsdr/capture.h"    /* bsdr_capture_decode_image_rgb — image-decode host service (faceswap) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>       /* _mkdir */
#define PLG_HANDLE       HMODULE
#define PLG_OPEN(p)      LoadLibraryA(p)
#define PLG_SYM(h,s)     ((void*)GetProcAddress((h),(s)))
#define PLG_CLOSE(h)     FreeLibrary(h)
#define PLG_EXT          ".dll"
#else
#include <dlfcn.h>
#define PLG_HANDLE       void*
#define PLG_OPEN(p)      dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define PLG_SYM(h,s)     dlsym((h),(s))
#define PLG_CLOSE(h)     dlclose(h)
#define PLG_EXT          ".so"
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define PLG_MAX 16
#define TAG "bsdr.plugin"

typedef struct {
    PLG_HANDLE         dl;
    const bsdr_plugin *p;
    void              *state;
} loaded_plugin;

static loaded_plugin   g_plugins[PLG_MAX];
static int             g_n = 0;
static bsdr_plugin_host g_host;
static void           *g_app = NULL;     /* remembered from the last load so reload can re-init */
/* Guards g_plugins/g_n so a live reload (webui thread) can't race the mic-keepalive query (botmic
 * thread) or an http/ui poll into freed plugin state. Lazily created on first load (single-threaded
 * at startup), then held around every list access. */
static bsdr_mutex     *g_lock = NULL;
static void plg_lock(void)   { if (g_lock) bsdr_mutex_lock(g_lock); }
static void plg_unlock(void) { if (g_lock) bsdr_mutex_unlock(g_lock); }

/* ---- host services exposed to plugins -------------------------------------------------------- */

static void host_log(int level, const char *tag, const char *fmt, ...) {
    /* Forward to the leveled logger. We can't pass the va_list straight to a "..." macro, so build
     * the message here and hand a single %s to bsdr_log. */
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    bsdr_log_level lv = (level <= 0) ? BSDR_LOG_DEBUG :
                        (level == 1) ? BSDR_LOG_INFO  :
                        (level == 2) ? BSDR_LOG_WARN  : BSDR_LOG_ERROR;
    bsdr_log(lv, tag ? tag : TAG, "%s", msg);
}

static int host_config_dir(char *buf, size_t cap) { return bsdr_config_dir(buf, cap) ? 1 : 0; }

/* bsdr_json_get_double returns bool; adapt to the plugin ABI's int-returning signature. */
static int host_json_get_double(const char *body, const char *key, double *out) {
    return bsdr_json_get_double(body, key, out) ? 1 : 0;
}
static int host_json_get_str(const char *body, const char *key, char *out, size_t cap) {
    return bsdr_json_get_str(body, key, out, cap) ? 1 : 0;
}

/* ---- per-plugin persisted config: <config_dir>/plugin_config/<ns>.conf (flat key=value) ------ */

/* Keep a namespace/key filesystem-safe: only [A-Za-z0-9._-], else refuse. */
static int cfg_safe(const char *s) {
    if (!s || !s[0] || strlen(s) > 128) return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_')) return 0;
    }
    return strstr(s, "..") ? 0 : 1;
}

static int cfg_ns_path(const char *ns, char *out, size_t cap) {
    if (!cfg_safe(ns)) return 0;
    char dir[768];
    if (!bsdr_config_dir(dir, sizeof dir)) return 0;
    char sub[900]; snprintf(sub, sizeof sub, "%s/plugin_config", dir);
    struct stat st;
    if (stat(sub, &st) != 0) {
#if defined(_WIN32)
        _mkdir(sub);
#else
        mkdir(sub, 0700);
#endif
    }
    snprintf(out, cap, "%s/%s.conf", sub, ns);
    return 1;
}

static int host_config_get(const char *ns, const char *key, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!cfg_safe(key)) return 0;
    char path[1000];
    if (!cfg_ns_path(ns, path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    size_t kl = strlen(key);
    const char *p = buf;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (l > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = l - kl - 1;
            while (vl && (p[kl + 1 + vl - 1] == '\r')) vl--;
            if (out && cap) { if (vl >= cap) vl = cap - 1; memcpy(out, p + kl + 1, vl); out[vl] = '\0'; }
            return 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

static int host_config_set(const char *ns, const char *key, const char *val) {
    if (!cfg_safe(key)) return 0;
    char path[1000];
    if (!cfg_ns_path(ns, path, sizeof path)) return 0;
    /* read existing, replacing the key */
    char out[8192]; size_t o = 0; out[0] = '\0';
    size_t kl = strlen(key);
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        const char *p = buf;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t l = eol ? (size_t)(eol - p) : strlen(p);
            int same = (l > kl && strncmp(p, key, kl) == 0 && p[kl] == '=');
            if (!same && l && o + l + 2 < sizeof out) { memcpy(out + o, p, l); o += l; out[o++] = '\n'; out[o] = '\0'; }
            if (!eol) break;
            p = eol + 1;
        }
    }
    /* append the new value (newlines in the value would corrupt the line format — flatten them) */
    char safeval[4096]; size_t vi = 0;
    for (const char *p = val ? val : ""; *p && vi < sizeof safeval - 1; p++)
        safeval[vi++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    safeval[vi] = '\0';
    if (o + kl + vi + 2 < sizeof out) o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\n", key, safeval);
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(out, 1, o, f);
    fclose(f);
#if !defined(_WIN32)
    chmod(path, 0600);
#endif
    return 1;
}

/* ---- bot host-service surface (ABI 4) -------------------------------------------------------- */
/* Thin wrappers over existing app/screenshot APIs, bound to the loaded app (g_app). Exposed via the
 * host table so a bot plugin (fullbot + friends) reaches roster/identity/moderation/desktop without
 * linking the host. See PLAN-bot-plugin.md §5. */
static size_t   host_roster_json(char *out, size_t cap)            { return g_app ? bsdr_app_roster_json((bsdr_app *)g_app, out, cap) : 0; }
static uint32_t host_owner_ssrc(void)                              { return g_app ? bsdr_app_owner_ssrc((bsdr_app *)g_app) : 0; }
static int      host_resolve_speaker(uint32_t ssrc, char *out, size_t cap) { return g_app ? bsdr_app_resolve_ssrc((bsdr_app *)g_app, ssrc, out, cap) : 0; }

/* ---- ABI 5: plugin-to-plugin services -----------------------------------------------------------
 * A publisher offers a named interface, its dependents look it up. We broker the pointer and never
 * interpret it — the meaning of each name is a contract between those plugins (see plugin.h). Small
 * fixed table: this is for a handful of bot services, not a general-purpose bus. Cleared on unload,
 * because every pointer belongs to a plugin we are about to dlclose(). */
#define PLG_MAX_SERVICES 16
static struct { char name[64]; void *iface; } g_services[PLG_MAX_SERVICES];
static int g_nservices;

static int host_service_publish(const char *name, void *iface) {
    if (!name || !name[0] || !iface || strlen(name) >= sizeof g_services[0].name) return 0;
    for (int i = 0; i < g_nservices; i++)            /* re-publishing a name replaces it */
        if (strcmp(g_services[i].name, name) == 0) { g_services[i].iface = iface; return 1; }
    if (g_nservices >= PLG_MAX_SERVICES) { BSDR_WARN(TAG, "service table full, '%s' not published", name); return 0; }
    snprintf(g_services[g_nservices].name, sizeof g_services[0].name, "%s", name);
    g_services[g_nservices].iface = iface;
    g_nservices++;
    BSDR_INFO(TAG, "plugin service '%s' published", name);
    return 1;
}

static void *host_service_get(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_nservices; i++)
        if (strcmp(g_services[i].name, name) == 0) return g_services[i].iface;
    return NULL;
}
static int      host_kick_user(const char *username)              { return (g_app && bsdr_app_kick_user((bsdr_app *)g_app, username)) ? 1 : 0; }
static int      host_ban_user(const char *username)               { return (g_app && bsdr_app_ban_user((bsdr_app *)g_app, username)) ? 1 : 0; }
static int      host_reset_room(void)                             { return g_app ? bsdr_app_reset_room((bsdr_app *)g_app) : 0; }
static int      host_screenshot_jpeg(int max_dim, unsigned char *out, size_t cap) { return bsdr_screenshot_jpeg(max_dim, out, cap); }
static void     host_speak(const char *text)                     { if (g_app) bsdr_app_tts_say((bsdr_app *)g_app, text); }
static int      host_stt(const int16_t *pcm, int frames, int rate, int channels, char *out, size_t cap) { return g_app ? bsdr_app_stt((bsdr_app *)g_app, pcm, frames, rate, channels, out, cap) : 0; }
static int      host_avatar_state(void)                          { return g_app ? bsdr_app_avatar_state((bsdr_app *)g_app) : 0; }
static void     host_set_voiceai_status(const char *status)      { if (g_app) bsdr_app_set_voiceai_status((bsdr_app *)g_app, status); }
static int      host_local_legacy_id(char *out, size_t cap)      { return g_app ? bsdr_app_local_legacy_id((bsdr_app *)g_app, out, cap) : 0; }
static int      host_llm_complete(const char *messages_json, const char *tools_json, char *out, size_t cap) { return g_app ? bsdr_app_llm_complete((bsdr_app *)g_app, messages_json, tools_json, out, cap) : 0; }
static void     host_utterance_subscribe(bsdr_utterance_cb cb, void *user) { if (g_app) bsdr_app_utterance_subscribe((bsdr_app *)g_app, cb, user); }
static void     host_avatar_set_follow(int on)                   { if (g_app) bsdr_app_set_bot_follow((bsdr_app *)g_app, on ? true : false); }
static int      host_avatar_follow(void)                         { return g_app ? bsdr_app_get_bot_follow((bsdr_app *)g_app) : 0; }
static void     host_bot_mode_register(const char *name, bsdr_bot_mode_cb cb, void *user) { if (g_app) bsdr_app_bot_mode_register((bsdr_app *)g_app, name, cb, user); }
static void     host_bot_mode_get(char *out, size_t cap)         { if (g_app) bsdr_app_bot_mode_get((bsdr_app *)g_app, out, cap); else if (out && cap) out[0] = '\0'; }
static void     host_avatar_enable(int on)                       { if (g_app) bsdr_app_set_avatar_enabled((bsdr_app *)g_app, on); }
static void     host_audio_gain_policy(bsdr_gain_policy_fn fn, void *user) { if (g_app) bsdr_app_set_gain_policy((bsdr_app *)g_app, fn, user); }
static void     host_input_type(const char *text)                { if (g_app) bsdr_app_input_type((bsdr_app *)g_app, text); }
static void     host_input_key(const char *keys)                 { if (g_app) bsdr_app_input_key((bsdr_app *)g_app, keys); }
static void     host_input_click(double x, double y, const char *button) { if (g_app) bsdr_app_input_click((bsdr_app *)g_app, x, y, button); }
static void     host_input_scroll(int amount)                    { if (g_app) bsdr_app_input_scroll((bsdr_app *)g_app, amount); }
static void     host_audio_fx_register(bsdr_audio_fx_fn fn, void *user) { bsdr_mediafx_set_audio(fn, user); }
static void     host_video_fx_register(bsdr_video_fx_fn fn, void *user, int order) { bsdr_mediafx_video_add(fn, user, order); }
static void     host_video_src_register(const bsdr_video_src_fx *fx) {
    bsdr_mediafx_set_video_src(fx);
    /* Re-open any live capture so the encoder is (re)sized from the new dims (or back to plain 2D). */
    if (g_app) bsdr_app_recapture((bsdr_app *)g_app);
}
static void     host_faceswap_config(int *on, int *tier, char *source, size_t source_cap, int *detect_every) {
    if (on) *on = 0;
    if (tier) *tier = 0;
    if (source && source_cap) source[0] = 0;
    if (detect_every) *detect_every = 0;
    if (!g_app) return;
    bool b = false; int t = 0;
    bsdr_app_get_faceswap((bsdr_app *)g_app, &b, &t, source, source_cap);
    if (on) *on = b ? 1 : 0;
    if (tier) *tier = t;
    if (detect_every) *detect_every = ((bsdr_app *)g_app)->faceswap_detect_every;
}
static int      host_decode_image_rgb(const char *path, uint8_t **rgb, int *w, int *h) {
    return bsdr_capture_decode_image_rgb(path, rgb, w, h);
}
static void     host_threed_config(int *mode, int *deepness, int *convergence, int *swap, int *full,
                                   int *tier, char *ai_cmd, size_t ai_cap) {
    if (mode) *mode = 0;
    if (deepness) *deepness = 0;
    if (convergence) *convergence = 0;
    if (swap) *swap = 0;
    if (full) *full = 0;
    if (tier) *tier = 0;
    if (ai_cmd && ai_cap) ai_cmd[0] = 0;
    if (g_app) bsdr_app_get_threed((bsdr_app *)g_app, mode, deepness, convergence, swap, full, tier, ai_cmd, ai_cap);
}
static int      host_depth_model_params(int tier, char *name, size_t name_cap, int *input_size, float mean[3], float std[3]) {
    const bsdr_model_info *mi = bsdr_model_for_tier(tier);
    if (!mi) return -1;
    if (name && name_cap) snprintf(name, name_cap, "%s", mi->name ? mi->name : "");
    if (input_size) *input_size = mi->input_size;
    if (mean) for (int i = 0; i < 3; i++) mean[i] = mi->mean[i];
    if (std)  for (int i = 0; i < 3; i++) std[i]  = mi->std[i];
    return 0;
}
static int      host_depth_model_resolve(int tier, int allow_download, char *path, size_t cap) {
    return bsdr_model_resolve(tier, allow_download, path, cap);
}
static int      host_depth_model_download_start(int tier) { return bsdr_model_download_start(tier); }
static void     host_depth_fx_register(bsdr_depth_fx_fn fn, void *user) { bsdr_mediafx_set_depth(fn, user); }
static void     host_face_fx_register(const bsdr_face_fx *fx) { bsdr_mediafx_set_face(fx); }
/* Model-store host services (voicestore/model_store stay in core). Normalize to "1=success, out filled"
 * regardless of the underlying return convention by checking that out was written. */
static int      host_model_dir(char *out, size_t cap)            { if (out && cap) out[0] = '\0'; bsdr_model_dir(out, cap); return (out && out[0]) ? 1 : 0; }
static int      host_voice_path(const char *id, char *out, size_t cap) { if (out && cap) out[0] = '\0'; bsdr_voice_path(id, out, cap); return (out && out[0]) ? 1 : 0; }
static int      host_voice_base_path(int which, char *out, size_t cap) { if (out && cap) out[0] = '\0'; bsdr_voice_base_path((bsdr_vbase)which, out, cap); return (out && out[0]) ? 1 : 0; }
static int      host_voice_base_ready(void)                      { return bsdr_voice_base_ready() ? 1 : 0; }
static size_t   host_voice_list_json(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    bsdr_voice_entry v[64];
    int n = bsdr_voice_list(v, (int)(sizeof v / sizeof v[0]));
    if (n < 0) n = 0;
    size_t o = (size_t)snprintf(out, cap, "[");
    for (int i = 0; i < n && o < cap - 200; i++) {
        char ide[160], nme[256];
        bsdr_json_escape(ide, sizeof ide, v[i].id);
        bsdr_json_escape(nme, sizeof nme, v[i].name);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"id\":\"%s\",\"name\":\"%s\",\"sr\":%d}",
                              i ? "," : "", ide, nme, v[i].sample_rate);
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

/* ---- plugin directory search ---------------------------------------------------------------- */

/* Absolute directory containing the running executable (best effort; empty on failure). */
static void exe_dir(char *out, size_t cap) {
    out[0] = '\0';
    char path[1024] = {0};
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path) return;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) return;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return;
    path[n] = '\0';
#endif
    char *slash = strrchr(path, '/');
#if defined(_WIN32)
    char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) { size_t len = (size_t)(slash - path); if (len < cap) { memcpy(out, path, len); out[len] = '\0'; } }
}

static int dir_has_plugins(const char *dir) {
    struct stat st;
    return dir && dir[0] && stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Pick the first existing plugin directory: $BSDR_PLUGIN_DIR, then dev/build and installed layouts,
 * then the per-user config dir. Writes into `out`; returns 1 if one was found. */
static int resolve_plugin_dir(char *out, size_t cap) {
    const char *env = getenv("BSDR_PLUGIN_DIR");
    if (env && env[0]) { snprintf(out, cap, "%s", env); return dir_has_plugins(out); }

    char exe[1024]; exe_dir(exe, sizeof exe);
    const char *rel[] = { "plugins", "../lib/bsdrX/plugins", "../share/bsdrX/plugins" };
    if (exe[0]) {
        for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
            snprintf(out, cap, "%s/%s", exe, rel[i]);
            if (dir_has_plugins(out)) return 1;
        }
    }
    snprintf(out, cap, "build/plugins");        /* running from the repo root during development */
    if (dir_has_plugins(out)) return 1;

    char cfg[768];
    if (bsdr_config_dir(cfg, sizeof cfg)) { snprintf(out, cap, "%s/plugins", cfg); if (dir_has_plugins(out)) return 1; }
    return 0;
}

/* ---- load / unload -------------------------------------------------------------------------- */

/* A plugin that has been opened + validated but not yet init'd. We collect ALL candidates from every
 * scanned directory first, then init them in dependency order (see resolve_and_init) so a plugin's
 * declared dependencies are guaranteed loaded before it. status: 0=pending, 1=loaded, -1=skipped. */
#define CAND_MAX (PLG_MAX * 2)
typedef struct {
    PLG_HANDLE         dl;
    const bsdr_plugin *p;
    int                status;
} plg_cand;

/* Index of the candidate whose descriptor name matches, else -1. */
static int cand_find(const plg_cand *c, int nc, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < nc; i++)
        if (c[i].p && c[i].p->name && strcmp(c[i].p->name, name) == 0) return i;
    return -1;
}

/* Number of dependency names this plugin declares (0 if it predates ABI 3 or declares none). */
static int cand_ndeps(const bsdr_plugin *p) {
    if (!(BSDR_PLUGIN_HAS(p, dep_count) && BSDR_PLUGIN_HAS(p, deps))) return 0;
    if (!p->deps || p->dep_count <= 0) return 0;
    return p->dep_count;
}

/* Open + validate one shared object and, if it passes, append it as a pending candidate (does NOT
 * init — that happens later in dependency order). Skips ABI-incompatible, capped, malformed, disabled,
 * or duplicate plugins exactly as before, closing their handle. */
static void gather_one(const char *fullpath, plg_cand *c, int *nc) {
    if (*nc >= CAND_MAX) { BSDR_WARN(TAG, "candidate limit (%d) reached, skipping %s", CAND_MAX, fullpath); return; }
    PLG_HANDLE dl = PLG_OPEN(fullpath);
    if (!dl) {
#if defined(_WIN32)
        BSDR_WARN(TAG, "load failed: %s", fullpath);
#else
        BSDR_WARN(TAG, "load failed: %s (%s)", fullpath, dlerror());
#endif
        return;
    }
    const bsdr_plugin *(*reg)(void) = (const bsdr_plugin *(*)(void))PLG_SYM(dl, "bsdr_plugin_register");
    if (!reg) { BSDR_WARN(TAG, "no bsdr_plugin_register in %s", fullpath); PLG_CLOSE(dl); return; }
    const bsdr_plugin *p = reg();
    /* Compatibility is the plugin's own range [N .. X]: N = p->abi (min host ABI it needs), X =
     * p->abi_max (last host ABI it supports; 0 = unbounded, any host >= N). Load only when this host
     * (BSDR_PLUGIN_ABI) satisfies  ABI_MIN <= N <= host  AND  (X == 0 || host <= X). Anything outside
     * is skipped, never called, so a plugin that needs a newer host — or one the author capped below
     * us — can't crash the host. abi_max is read only if the plugin actually provides it. */
    if (!p || p->abi < BSDR_PLUGIN_ABI_MIN || p->abi > BSDR_PLUGIN_ABI) {
        BSDR_WARN(TAG, "incompatible plugin %s (needs host abi>=%d; this host is %d, floor %d)",
                  fullpath, p ? p->abi : -1, BSDR_PLUGIN_ABI, BSDR_PLUGIN_ABI_MIN);
        PLG_CLOSE(dl); return;
    }
    if (BSDR_PLUGIN_HAS(p, abi_max) && p->abi_max != 0 && BSDR_PLUGIN_ABI > p->abi_max) {
        BSDR_WARN(TAG, "plugin %s is capped at host abi %d but this host is %d — skipping (obsolete)",
                  fullpath, p->abi_max, BSDR_PLUGIN_ABI);
        PLG_CLOSE(dl); return;
    }
    /* struct_size must at least cover the required header (abi + struct_size + name); anything less
     * means a malformed/foreign descriptor we must not read further. */
    if (p->struct_size < offsetof(bsdr_plugin, name) + sizeof p->name || !p->name || !p->name[0]) {
        BSDR_WARN(TAG, "bad plugin descriptor %s (struct_size=%zu, no name)", fullpath, p ? p->struct_size : 0);
        PLG_CLOSE(dl); return;
    }
    /* Operator disabled this plugin in the web panel: skip it (stays on disk, just not loaded). Note a
     * disabled plugin is simply absent from the candidate set, so anything depending on it is skipped
     * too (its dependency is "unavailable") — you can't run a plugin without its dependency, even if the
     * dependency was only disabled. */
    if (bsdr_plugin_name_disabled(p->name)) {
        BSDR_INFO(TAG, "plugin '%s' is disabled — skipping %s", p->name, fullpath);
        PLG_CLOSE(dl); return;
    }
    if (cand_find(c, *nc, p->name) >= 0) { BSDR_WARN(TAG, "duplicate plugin '%s' ignored", p->name); PLG_CLOSE(dl); return; }

    c[*nc].dl = dl; c[*nc].p = p; c[*nc].status = 0; (*nc)++;
}

/* Init the gathered candidates in dependency order: a plugin is init'd (and registered into g_plugins)
 * only once every plugin it names in `deps` has itself loaded successfully — so a dependency is always
 * up before its dependent, and shutdown (reverse order) tears the dependent down first. A candidate is
 * SKIPPED (never init'd) when a declared dependency is missing/disabled/incompatible, when a dependency
 * failed to init, or when it sits in a dependency cycle. Repeatedly makes a pass until no more progress;
 * whatever is still pending after that is cyclic/unresolved. Closes the handle of every skipped one. */
static void resolve_and_init(plg_cand *c, int nc) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < nc; i++) {
            if (c[i].status != 0) continue;                 /* already loaded or skipped */
            const bsdr_plugin *p = c[i].p;
            int nd = cand_ndeps(p), missing = 0, blocked = 0;
            const char *missname = NULL;
            for (int k = 0; k < nd; k++) {
                const char *dn = p->deps[k];
                if (!dn || !dn[0]) continue;                /* tolerate an empty slot in the array */
                int j = cand_find(c, nc, dn);
                if (j < 0 || c[j].status == -1) { missing = 1; missname = dn; break; }
                if (c[j].status == 0) blocked = 1;          /* dep present but not yet loaded */
            }
            if (missing) {
                BSDR_WARN(TAG, "plugin '%s' needs dependency '%s' which is unavailable — skipping", p->name, missname);
                c[i].status = -1; progress = 1; continue;
            }
            if (blocked) continue;                          /* wait for a dependency to load first */
            if (g_n >= PLG_MAX) {
                BSDR_WARN(TAG, "plugin limit (%d) reached, skipping '%s'", PLG_MAX, p->name);
                c[i].status = -1; progress = 1; continue;
            }
            void *state = NULL;
            if (BSDR_PLUGIN_HAS(p, init) && p->init && p->init(&g_host, &state) != 0) {
                BSDR_WARN(TAG, "plugin '%s' init failed — skipping it and anything that depends on it", p->name);
                c[i].status = -1; progress = 1; continue;
            }
            g_plugins[g_n].dl = c[i].dl; g_plugins[g_n].p = p; g_plugins[g_n].state = state; g_n++;
            c[i].status = 1; progress = 1;
            BSDR_INFO(TAG, "loaded plugin '%s' v%s — %s", p->name, p->version ? p->version : "?", p->description ? p->description : "");
        }
    }
    /* Anything still pending has all deps present yet never became ready => a dependency cycle. Skip it
     * and release the handle of every candidate we didn't register into g_plugins. */
    for (int i = 0; i < nc; i++) {
        if (c[i].status == 0) {
            BSDR_WARN(TAG, "plugin '%s' not loaded — dependency cycle or unresolved dependency", c[i].p->name);
            c[i].status = -1;
        }
        if (c[i].status == -1 && c[i].dl) { PLG_CLOSE(c[i].dl); c[i].dl = NULL; }
    }
}

/* The per-user, always-writable plugin dir (<config_dir>/plugins). Downloaded plugins install here and
 * the loader always scans it (in addition to the resolved dev/install dir). Created on demand. */
int bsdr_plugins_user_dir(char *out, size_t cap) {
    char cfg[768];
    if (!bsdr_config_dir(cfg, sizeof cfg)) return 0;
    snprintf(out, cap, "%s/plugins", cfg);
    struct stat st;
    if (stat(out, &st) != 0) {
#if defined(_WIN32)
        _mkdir(out);
#else
        mkdir(out, 0700);
#endif
    }
    return 1;
}

/* Scan one directory (non-recursive): open + validate every *.so/.dll into the candidate set. Init
 * happens later, once all directories are gathered, so cross-directory dependencies resolve. */
static void gather_dir(const char *dir, plg_cand *c, int *nc) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t extlen = strlen(PLG_EXT);
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t nl = strlen(de->d_name);
        if (nl <= extlen || strcmp(de->d_name + nl - extlen, PLG_EXT) != 0) continue;
        char full[1200]; snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        gather_one(full, c, nc);
    }
    closedir(d);
}

/* Actual load work; assumes the lock is held (or that we're single-threaded at startup). */
static void plugins_load_locked(void) {
    g_host.abi = BSDR_PLUGIN_ABI;
    g_host.struct_size = sizeof g_host;
    g_host.app = g_app;
    g_host.log = host_log;
    g_host.http_respond = bsdr_webui_plugin_respond;
    g_host.json_get_double = host_json_get_double;
    g_host.config_dir = host_config_dir;
    g_host.json_get_str = host_json_get_str;   /* appended in ABI 2 */
    g_host.config_get = host_config_get;
    g_host.config_set = host_config_set;
    /* ABI 4 bot host-service surface. The registry functions match the host vtable signatures exactly,
     * so they're assigned directly (no shim). A plugin's tools are dropped when it unloads (below). */
    g_host.tool_register   = bsdr_tools_register;
    g_host.tool_unregister = bsdr_tools_unregister;
    g_host.tool_list_json  = bsdr_tools_list_json;
    g_host.tool_invoke     = bsdr_tools_invoke;
    g_host.roster_json     = host_roster_json;
    g_host.owner_ssrc      = host_owner_ssrc;
    g_host.resolve_speaker = host_resolve_speaker;
    g_host.kick_user       = host_kick_user;
    g_host.ban_user        = host_ban_user;
    g_host.reset_room      = host_reset_room;
    g_host.screenshot_jpeg = host_screenshot_jpeg;
    g_host.speak                = host_speak;
    g_host.stt                  = host_stt;
    g_host.avatar_state         = host_avatar_state;
    g_host.local_legacy_user_id = host_local_legacy_id;
    g_host.avatar_set_follow    = host_avatar_set_follow;
    g_host.avatar_follow        = host_avatar_follow;
    g_host.llm_complete         = host_llm_complete;
    g_host.json_escape          = bsdr_json_escape;
    g_host.utterance_subscribe  = host_utterance_subscribe;
    g_host.service_publish      = host_service_publish;   /* ABI 5 */
    g_host.service_get          = host_service_get;
    g_host.bot_mode_register    = host_bot_mode_register;
    g_host.bot_mode_get         = host_bot_mode_get;
    g_host.avatar_enable        = host_avatar_enable;
    g_host.audio_gain_policy    = host_audio_gain_policy;
    g_host.input_type           = host_input_type;
    g_host.input_key            = host_input_key;
    g_host.input_click          = host_input_click;
    g_host.input_scroll         = host_input_scroll;
    g_host.audio_fx_register    = host_audio_fx_register;
    g_host.video_fx_register    = host_video_fx_register;
    g_host.video_src_register   = host_video_src_register;
    g_host.faceswap_config      = host_faceswap_config;
    g_host.decode_image_rgb     = host_decode_image_rgb;
    g_host.threed_config              = host_threed_config;
    g_host.depth_model_params         = host_depth_model_params;
    g_host.depth_model_resolve        = host_depth_model_resolve;
    g_host.depth_model_download_start = host_depth_model_download_start;
    g_host.depth_fx_register          = host_depth_fx_register;
    g_host.face_fx_register           = host_face_fx_register;
    g_host.model_dir            = host_model_dir;
    g_host.voice_path           = host_voice_path;
    g_host.voice_base_path      = host_voice_base_path;
    g_host.voice_base_ready     = host_voice_base_ready;
    g_host.voice_list_json      = host_voice_list_json;
    g_host.set_voiceai_status   = host_set_voiceai_status;   /* ABI 7 */

    /* Gather candidates from every scanned directory FIRST, then init them in dependency order. */
    plg_cand cand[CAND_MAX];
    int nc = 0;

    char dir[1024];
    int scanned = 0;
    if (resolve_plugin_dir(dir, sizeof dir)) { gather_dir(dir, cand, &nc); scanned = 1; }

    /* Always also scan the per-user dir, where store-downloaded plugins land — the resolved dir above
     * may be a read-only dev/install path that never sees downloads (dedup-by-name in gather_one keeps a
     * plugin present in both dirs from being gathered twice). */
    char udir[1024];
    if (bsdr_plugins_user_dir(udir, sizeof udir) && (!scanned || strcmp(udir, dir) != 0)) gather_dir(udir, cand, &nc);

    resolve_and_init(cand, nc);

    if (!scanned && !g_n) BSDR_DEBUG(TAG, "no plugin directory found; none loaded");
}

static void plugins_unload_locked(void) {
    /* Deactivate + drop any plugin-registered bot modes (fires the active mode's on_active(0), which
     * unsubscribes hearing + disables the avatar) and then release any lingering hearing subscription —
     * all BEFORE unmapping code, so nothing calls into an unloaded plugin. */
    if (g_app) {
        bsdr_app_bot_mode_clear_plugin_modes((bsdr_app *)g_app);
        bsdr_app_utterance_subscribe((bsdr_app *)g_app, NULL, NULL);
        bsdr_app_set_gain_policy((bsdr_app *)g_app, NULL, NULL);
    }
    bsdr_mediafx_set_audio(NULL, NULL);   /* drop any media effect before unmapping (waits for in-flight) */
    if (bsdr_mediafx_video_src_active()) {   /* drop a dim-changing 2D->3D transform + reopen capture to 2D */
        bsdr_mediafx_set_video_src(NULL);
        if (g_app) bsdr_app_recapture((bsdr_app *)g_app);
    }
    bsdr_mediafx_set_depth(NULL, NULL);   /* drop any depth estimator before unmapping (waits in-flight) */
    bsdr_mediafx_set_face(NULL);          /* drop any RGB face-swap interface before unmapping */
    /* Every published service points INTO a plugin we are about to dlclose(), so the table cannot
     * outlive them. Cleared wholesale here rather than per-plugin: unload takes all of them anyway,
     * and a dangling iface would be called straight into unmapped code. */
    memset(g_services, 0, sizeof g_services);
    g_nservices = 0;
    for (int i = g_n - 1; i >= 0; i--) {
        const bsdr_plugin *p = g_plugins[i].p;
        /* Drop any tools this plugin registered (keyed by its state) BEFORE its code is unmapped, so a
         * stale handler can never be invoked after unload/reload. */
        bsdr_tools_unregister_owner(g_plugins[i].state);
        /* Same for any video-fx chain entry (keyed by state); waits for an in-flight frame. */
        bsdr_mediafx_video_remove_owner(g_plugins[i].state);
        if (BSDR_PLUGIN_HAS(p, shutdown) && p->shutdown) p->shutdown(g_plugins[i].state);
        if (g_plugins[i].dl) PLG_CLOSE(g_plugins[i].dl);
        memset(&g_plugins[i], 0, sizeof g_plugins[i]);
    }
    g_n = 0;
}

void bsdr_plugins_load(void *app) {
    if (!g_lock) g_lock = bsdr_mutex_new();   /* first call is at startup, before other threads */
    plg_lock();
    g_app = app;
    plugins_load_locked();
    plg_unlock();
}

void bsdr_plugins_unload(void) {
    plg_lock();
    plugins_unload_locked();
    plg_unlock();
}

void bsdr_plugins_reload(void) {
    plg_lock();
    plugins_unload_locked();
    plugins_load_locked();
    plg_unlock();
    BSDR_INFO(TAG, "plugins reloaded (%d loaded)", g_n);
}

int bsdr_plugins_count(void) { plg_lock(); int n = g_n; plg_unlock(); return n; }

int bsdr_plugins_is_loaded(const char *name) {
    if (!name || !name[0]) return 0;
    plg_lock();
    int found = 0;
    for (int i = 0; i < g_n; i++)
        if (g_plugins[i].p && g_plugins[i].p->name && strcmp(g_plugins[i].p->name, name) == 0) { found = 1; break; }
    plg_unlock();
    return found;
}

/* ---- dispatch ------------------------------------------------------------------------------- */

int bsdr_plugins_http(const char *method, const char *path, const char *body, void *conn) {
    plg_lock();
    int handled = 0;
    for (int i = 0; i < g_n; i++) {
        if (!(BSDR_PLUGIN_HAS(g_plugins[i].p, http) && g_plugins[i].p->http)) continue;
        /* Only offer a request under this plugin's own /api/plugin/<name>/ namespace. */
        char pfx[160]; int m = snprintf(pfx, sizeof pfx, "/api/plugin/%s/", g_plugins[i].p->name);
        if (m > 0 && strncmp(path, pfx, (size_t)m) == 0)
            if (g_plugins[i].p->http(g_plugins[i].state, method, path, body, conn)) { handled = 1; break; }
    }
    plg_unlock();
    return handled;
}

/* JSON array of the loaded plugins' descriptor names, e.g. ["fullbot","faceswap"]. The web UI gates
 * plugin-backed feature cards (2D->3D, face swap, voice changer, voice assistant) on this so a card only
 * appears when its plugin is actually loaded. */
size_t bsdr_plugins_names_json(char *out, size_t cap) {
    plg_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < g_n && o < cap; i++) {
        if (!(g_plugins[i].p && g_plugins[i].p->name)) continue;
        char nm[160]; bsdr_json_escape(nm, sizeof nm, g_plugins[i].p->name);
        o += (size_t)snprintf(out + o, cap - o, "%s\"%s\"", emitted ? "," : "", nm);
        emitted++;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    plg_unlock();
    return o;
}

/* Read a plugin's DESCRIPTOR name (and version) straight off disk without loading it into the host: the
 * loader keys everything — is_loaded, the disabled list — on the descriptor `name`, which for built-ins
 * differs from the filename (bot_moderator.so → "bot-moderator"). So the installed list must key on the
 * descriptor too, or its Enable/Disable would write a name the loader never matches. dlopen is refcounted,
 * so peeking a plugin that is already loaded is harmless (this open/close nets to zero). Returns 1 on a
 * valid plugin. */
static int plg_peek(const char *path, char *name, size_t ncap, char *ver, size_t vcap) {
    name[0] = ver[0] = '\0';
    PLG_HANDLE dl = PLG_OPEN(path);
    if (!dl) return 0;
    const bsdr_plugin *(*reg)(void) = (const bsdr_plugin *(*)(void))PLG_SYM(dl, "bsdr_plugin_register");
    const bsdr_plugin *p = reg ? reg() : NULL;
    int ok = 0;
    if (p && p->name && p->name[0]) {
        snprintf(name, ncap, "%s", p->name);
        if (BSDR_PLUGIN_HAS(p, version) && p->version) snprintf(ver, vcap, "%s", p->version);
        ok = 1;
    }
    PLG_CLOSE(dl);
    return ok;
}

/* JSON array of EVERY plugin present on disk — the app/dev dir (built-in) and the per-user store dir —
 * each with its live state, for the web UI's "installed plugins" list:
 *   [{"name","loaded","enabled","builtin","version"},…]
 * `name` is the descriptor name (what Enable/Disable must use); `enabled` is the operator's on/off from
 * plugins.disabled (a disabled plugin stays on disk, just isn't loaded); `loaded` is whether it is mapped
 * right now; `builtin` means it ships with the app (app dir) rather than being store-installed. Names are
 * de-duplicated (a plugin in both dirs is listed once, marked builtin). */
size_t bsdr_plugins_installed_json(char *out, size_t cap) {
    char seen[PLG_MAX * 2][64];
    int nseen = 0;
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    int emitted = 0;
    /* app/dev dir first (so a name in both is marked builtin), then the per-user store dir. */
    char dirs[2][1024];
    int builtin[2] = { 1, 0 }, nd = 0;
    if (resolve_plugin_dir(dirs[nd], sizeof dirs[0])) nd++;
    char udir[1024];
    if (bsdr_plugins_user_dir(udir, sizeof udir) && (nd == 0 || strcmp(udir, dirs[0]) != 0)) {
        snprintf(dirs[nd], sizeof dirs[0], "%s", udir); builtin[nd] = 0; nd++;
    }
    size_t extlen = strlen(PLG_EXT);
    for (int di = 0; di < nd; di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && o < cap - 320 && nseen < (int)(sizeof seen / sizeof seen[0])) {
            size_t fnl = strlen(de->d_name);
            if (fnl <= extlen || strcmp(de->d_name + fnl - extlen, PLG_EXT) != 0) continue;
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", dirs[di], de->d_name);
            char nm[64], ver[64];
            if (!plg_peek(path, nm, sizeof nm, ver, sizeof ver)) continue;   /* not a valid plugin */
            int dup = 0;
            for (int k = 0; k < nseen; k++) if (strcmp(seen[k], nm) == 0) { dup = 1; break; }
            if (dup) continue;
            snprintf(seen[nseen++], sizeof seen[0], "%s", nm);
            char ne[160], ve[128];
            bsdr_json_escape(ne, sizeof ne, nm);
            bsdr_json_escape(ve, sizeof ve, ver);
            o += (size_t)snprintf(out + o, cap - o,
                "%s{\"name\":\"%s\",\"loaded\":%s,\"enabled\":%s,\"builtin\":%s,\"version\":\"%s\"}",
                emitted ? "," : "", ne,
                bsdr_plugins_is_loaded(nm) ? "true" : "false",
                bsdr_plugin_name_disabled(nm) ? "false" : "true",
                builtin[di] ? "true" : "false", ve);
            emitted++;
        }
        closedir(d);
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

size_t bsdr_plugins_ui_json(char *out, size_t cap) {
    plg_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < g_n && o < cap; i++) {
        if (!(BSDR_PLUGIN_HAS(g_plugins[i].p, ui_html) && g_plugins[i].p->ui_html)) continue;
        char html[8192]; size_t hl = 0; html[0] = '\0';
        g_plugins[i].p->ui_html(g_plugins[i].state, html, sizeof html, &hl);
        if (hl >= sizeof html) hl = sizeof html - 1;
        html[hl] = '\0';
        char esc[16384]; bsdr_json_escape(esc, sizeof esc, html);
        char nm[160]; bsdr_json_escape(nm, sizeof nm, g_plugins[i].p->name);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"html\":\"%s\"}", emitted ? "," : "", nm, esc);
        emitted++;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    plg_unlock();
    return o;
}

size_t bsdr_plugins_panel_json(char *out, size_t cap) {
    plg_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < g_n && o < cap; i++) {
        if (!(BSDR_PLUGIN_HAS(g_plugins[i].p, panel_html) && g_plugins[i].p->panel_html)) continue;
        char html[16384]; size_t hl = 0; html[0] = '\0';
        g_plugins[i].p->panel_html(g_plugins[i].state, html, sizeof html, &hl);
        if (hl >= sizeof html) hl = sizeof html - 1;
        html[hl] = '\0';
        char esc[32768]; bsdr_json_escape(esc, sizeof esc, html);
        char nm[160]; bsdr_json_escape(nm, sizeof nm, g_plugins[i].p->name);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"html\":\"%s\"}", emitted ? "," : "", nm, esc);
        emitted++;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    plg_unlock();
    return o;
}

size_t bsdr_plugins_sections_json(char *out, size_t cap) {
    plg_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < g_n && o < cap; i++) {
        if (!(BSDR_PLUGIN_HAS(g_plugins[i].p, sections_html) && g_plugins[i].p->sections_html)) continue;
        char html[16384]; size_t hl = 0; html[0] = '\0';
        g_plugins[i].p->sections_html(g_plugins[i].state, html, sizeof html, &hl);
        if (hl >= sizeof html) hl = sizeof html - 1;
        html[hl] = '\0';
        if (!html[0]) continue;
        char esc[32768]; bsdr_json_escape(esc, sizeof esc, html);
        char nm[160]; bsdr_json_escape(nm, sizeof nm, g_plugins[i].p->name);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"html\":\"%s\"}", emitted ? "," : "", nm, esc);
        emitted++;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    plg_unlock();
    return o;
}

size_t bsdr_plugins_scripts_json(char *out, size_t cap) {
    plg_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < g_n && o < cap; i++) {
        const bsdr_plugin *p = g_plugins[i].p;
        if (!(BSDR_PLUGIN_HAS(p, ui_script) && p->ui_script && p->ui_script[0])) continue;
        char nm[160]; bsdr_json_escape(nm, sizeof nm, p->name);
        char se[1024]; bsdr_json_escape(se, sizeof se, p->ui_script);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"src\":\"%s\"}", emitted ? "," : "", nm, se);
        emitted++;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    plg_unlock();
    return o;
}

size_t bsdr_plugins_config_json(char *out, size_t cap) {
    plg_lock();
    size_t o = 0; int emitted = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < g_n && o < cap; i++) {
        const bsdr_plugin *p = g_plugins[i].p;
        if (!(BSDR_PLUGIN_HAS(p, config) && p->config &&
              BSDR_PLUGIN_HAS(p, config_count) && p->config_count > 0)) continue;
        char nm[160]; bsdr_json_escape(nm, sizeof nm, p->name);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"plugin\":\"%s\",\"vars\":[", emitted ? "," : "", nm);
        for (int j = 0; j < p->config_count && o < cap - 400; j++) {
            const bsdr_plugin_cfgvar *cv = &p->config[j];
            if (!cv->key || !cv->key[0]) continue;
            char val[2048];
            if (!host_config_get(p->name, cv->key, val, sizeof val))
                snprintf(val, sizeof val, "%s", cv->def ? cv->def : "");
            char ke[160], le[256], ve[4096], he[512];
            bsdr_json_escape(ke, sizeof ke, cv->key);
            bsdr_json_escape(le, sizeof le, cv->label ? cv->label : cv->key);
            bsdr_json_escape(ve, sizeof ve, val);
            bsdr_json_escape(he, sizeof he, cv->help ? cv->help : "");
            o += (size_t)snprintf(out + o, cap - o,
                "%s{\"key\":\"%s\",\"label\":\"%s\",\"type\":\"%s\",\"value\":\"%s\",\"help\":\"%s\"}",
                j ? "," : "", ke, le, cv->type ? cv->type : "text", ve, he);
        }
        o += (size_t)snprintf(out + o, cap - o, "]}");
        emitted++;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    plg_unlock();
    return o;
}

int bsdr_plugins_config_set(const char *plugin, const char *key, const char *val) {
    if (!plugin || !key) return 0;
    plg_lock();
    int ok = 0;
    for (int i = 0; i < g_n; i++) {
        const bsdr_plugin *p = g_plugins[i].p;
        if (!p->name || strcmp(p->name, plugin) != 0) continue;
        if (!(BSDR_PLUGIN_HAS(p, config) && p->config && BSDR_PLUGIN_HAS(p, config_count))) break;
        for (int j = 0; j < p->config_count; j++)
            if (p->config[j].key && strcmp(p->config[j].key, key) == 0) {   /* only declared keys */
                ok = host_config_set(p->name, key, val ? val : "");
                break;
            }
        break;
    }
    plg_unlock();
    return ok;
}

int bsdr_plugins_mic_keepalive_period_ms(int def) {
    plg_lock();
    int r = def;
    for (int i = 0; i < g_n; i++) {
        if (!(BSDR_PLUGIN_HAS(g_plugins[i].p, mic_keepalive_period_ms) && g_plugins[i].p->mic_keepalive_period_ms)) continue;
        int v = g_plugins[i].p->mic_keepalive_period_ms(g_plugins[i].state);
        if (v > 0) { r = v; break; }
    }
    plg_unlock();
    return r;
}
