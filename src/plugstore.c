/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* In-app Plugin Store client — see bsdr/plugstore.h.
 *
 * Server-to-server calls to a bsdrx-plugstore instance: JSON auth (register/login -> license key),
 * catalog relay, purchase-URL composition, and download+install+reload of plugins for this
 * platform/arch/ABI. State (store URL, license key, email) persists in <config_dir>/plugstore.conf;
 * the per-plugin disabled list in <config_dir>/plugins.disabled. All entry points here run on the
 * single web-UI accept thread, so the in-memory config cache needs no lock. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE        /* dladdr + Dl_info (GNU extension) for onnx_provider_dir */
#endif
#include "bsdr/plugstore.h"
#include "bsdr/plugin.h"    /* bsdr_plugins_reload / _user_dir / _is_loaded */
#include "bsdr/app.h"       /* bsdr_config_dir */
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"  /* bsdr_mutex + bsdr_thread_start_detached for the async download */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#if !defined(_WIN32)
#include <unistd.h>     /* access(W_OK) */
#include <dlfcn.h>      /* locate an already-loaded libonnxruntime.so to find its (writable) lib dir */
#endif
#if defined(_WIN32)
#include <direct.h>       /* _mkdir */
#endif

#include "miniz.h"

#define TAG "bsdr.plugstore"

#if defined(_WIN32)
#define PLG_EXT ".dll"
#else
#define PLG_EXT ".so"
#endif

/* ---- platform/arch tags (must match what scripts/build-plugins.sh stamps) ------------------- */

static const char *ps_platform(void) {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

static const char *ps_arch(void) {
#if defined(__ANDROID__)
#  if defined(__aarch64__)
    return "arm64-v8a";
#  elif defined(__arm__)
    return "armeabi-v7a";
#  elif defined(__x86_64__)
    return "x86_64";
#  elif defined(__i386__)
    return "x86";
#  else
    return "unknown";
#  endif
#else
#  if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#  elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#  elif defined(__arm__)
    return "armv7";
#  elif defined(__i386__) || defined(_M_IX86)
    return "i686";
#  else
    return "unknown";
#  endif
#endif
}

/* ---- persistent config: <config_dir>/plugstore.conf ----------------------------------------- */

static char g_url[512]   = "";
static char g_token[256] = "";
static char g_email[256] = "";
static int  g_loaded     = 0;

static int cfg_path(char *out, size_t cap) {
    char dir[768];
    if (!bsdr_config_dir(dir, sizeof dir)) return 0;
    snprintf(out, cap, "%s/plugstore.conf", dir);
    return 1;
}

/* Copy the value for `key` out of a flat key=value buffer into out (empty if absent). */
static void cfg_field(const char *buf, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    size_t kl = strlen(key);
    const char *p = buf;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = linelen - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = '\0';
            return;
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static void ps_load_cfg(void) {
    if (g_loaded) return;
    g_loaded = 1;
    snprintf(g_url, sizeof g_url, "%s", BSDR_PLUGSTORE_DEFAULT_URL);
    char path[900];
    if (!cfg_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char v[512];
    cfg_field(buf, "url", v, sizeof v);   if (v[0]) snprintf(g_url, sizeof g_url, "%s", v);
    cfg_field(buf, "token", g_token, sizeof g_token);
    cfg_field(buf, "email", g_email, sizeof g_email);
}

static void ps_save_cfg(void) {
    char path[900];
    if (!cfg_path(path, sizeof path)) return;
    FILE *f = fopen(path, "wb");
    if (!f) { BSDR_WARN(TAG, "cannot write %s", path); return; }
    fprintf(f, "url=%s\ntoken=%s\nemail=%s\n", g_url, g_token, g_email);
    fclose(f);
#if !defined(_WIN32)
    chmod(path, 0600);   /* the license key is a credential */
#endif
}

/* Trim any trailing '/' so we can append "/api/v1/...". */
static void base_url(char *out, size_t cap) {
    ps_load_cfg();
    snprintf(out, cap, "%s", g_url);
    size_t l = strlen(out);
    while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
}

/* A plugin slug is only ever [a-z0-9._-]; refuse anything else so it can't inject into a URL/path. */
static int valid_slug(const char *s) {
    if (!s || !s[0] || strlen(s) > 128) return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
                 (c >= 'A' && c <= 'Z');
        if (!ok) return 0;
    }
    if (strstr(s, "..")) return 0;
    return 1;
}

/* ---- HTTP helpers --------------------------------------------------------------------------- */

/* POST a JSON body to <base><path>; returns the HTTP status (or -1 on transport failure) and copies
 * the response body into `bodyout`. */
static int ps_post(const char *apipath, const char *json, char *bodyout, size_t bodycap) {
    char url[700], base[512];
    base_url(base, sizeof base);
    snprintf(url, sizeof url, "%s%s", base, apipath);
    char resp[16384];
    int n = bsdr_http_request("POST", url, NULL, 0, "application/json",
                              json, strlen(json), resp, sizeof resp);
    if (n < 0) { if (bodyout) bodyout[0] = '\0'; return -1; }
    int st = bsdr_http_status(resp);
    const char *b = bsdr_http_body(resp);
    if (bodyout) snprintf(bodyout, bodycap, "%s", b ? b : "");
    return st;
}

/* GET <base><path> (adding the license-key bearer if logged in); writes the body into `bodyout`
 * (heap caller-provided). Returns the HTTP status, or -1 on transport failure. */
static int ps_get(const char *apipath, char *bodyout, size_t bodycap) {
    char url[900], base[512];
    base_url(base, sizeof base);
    snprintf(url, sizeof url, "%s%s", base, apipath);
    bsdr_http_header hdr[1];
    int nh = 0;
    char auth[300];
    if (g_token[0]) {
        snprintf(auth, sizeof auth, "Bearer %s", g_token);
        hdr[0].name = "Authorization"; hdr[0].value = auth; nh = 1;
    }
    /* Response can be large (whole catalog); use a generous heap buffer. */
    size_t cap = bodycap + 8192;
    char *resp = malloc(cap);
    if (!resp) return -1;
    int n = bsdr_http_request("GET", url, hdr, nh, NULL, NULL, 0, resp, cap);
    if (n < 0) { free(resp); if (bodyout) bodyout[0] = '\0'; return -1; }
    int st = bsdr_http_status(resp);
    const char *b = bsdr_http_body(resp);
    if (bodyout) snprintf(bodyout, bodycap, "%s", b ? b : "");
    free(resp);
    return st;
}

/* ---- account -------------------------------------------------------------------------------- */

/* Read a JSON boolean field. bsdr_json_get_double CANNOT — strtod("true") parses nothing — which is why a
 * successful {"ok":true} store reply was silently read as a failed login. Matches "key":true (any spacing). */
static int ps_json_bool(const char *json, const char *key) {
    char pat[80];
    int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return 0;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    return strncmp(p, "true", 4) == 0;
}

static int ps_auth(const char *apipath, const char *email, const char *pw, char *err, size_t errcap) {
    if (!email || !email[0] || !pw || !pw[0]) { if (err) snprintf(err, errcap, "email and password required"); return 0; }
    char ee[512], pe[512];
    bsdr_json_escape(ee, sizeof ee, email);
    bsdr_json_escape(pe, sizeof pe, pw);
    char body[1200];
    snprintf(body, sizeof body, "{\"email\":\"%s\",\"password\":\"%s\"}", ee, pe);
    char resp[8192];
    int st = ps_post(apipath, body, resp, sizeof resp);
    if (st < 0) { if (err) snprintf(err, errcap, "cannot reach the store (check the URL / network)"); return 0; }
    if (ps_json_bool(resp, "ok")) {
        char tok[256] = "", em[256] = "";
        bsdr_json_get_str(resp, "token", tok, sizeof tok);
        bsdr_json_get_str(resp, "email", em, sizeof em);
        if (!tok[0]) { if (err) snprintf(err, errcap, "store returned no license key"); return 0; }
        snprintf(g_token, sizeof g_token, "%s", tok);
        snprintf(g_email, sizeof g_email, "%s", em[0] ? em : email);
        ps_save_cfg();
        BSDR_INFO(TAG, "signed in to the plugin store as %s", g_email);
        return 1;
    }
    char msg[256] = "";
    bsdr_json_get_str(resp, "error", msg, sizeof msg);
    if (err) snprintf(err, errcap, "%s", msg[0] ? msg : "sign-in failed");
    /* Log the reason so a failed sign-in is diagnosable from debug.log, not just a UI alert: it
     * distinguishes wrong credentials (the store's own message) from a store that couldn't be reached. */
    BSDR_WARN(TAG, "plugin-store %s failed (HTTP %d): %s", apipath, st, msg[0] ? msg : "no error message");
    return 0;
}

int bsdr_plugstore_register(const char *email, const char *pw, char *err, size_t errcap) {
    return ps_auth("/api/v1/auth/register", email, pw, err, errcap);
}

int bsdr_plugstore_login(const char *email, const char *pw, char *err, size_t errcap) {
    return ps_auth("/api/v1/auth/login", email, pw, err, errcap);
}

/* License-key sign-in — the works-for-EVERY-account path. Any logged-in user (password OR OAuth, admin or
 * not) can mint a license key on the store's /account page and paste it here. The agent already uses the
 * key as its Bearer credential for the catalog + downloads; this just adopts a key directly and verifies
 * it via GET /api/v1/me. So it sidesteps password transport entirely and works for OAuth accounts (which
 * have no usable password) and admins alike. */
int bsdr_plugstore_login_key(const char *key, char *err, size_t errcap) {
    ps_load_cfg();
    char k[256];
    snprintf(k, sizeof k, "%s", key ? key : "");
    char *s = k;                                  /* trim whitespace a paste often brings along */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    if (!s[0]) { if (err) snprintf(err, errcap, "paste a license key"); return 0; }
    char old[256];
    snprintf(old, sizeof old, "%s", g_token);     /* restore on failure */
    snprintf(g_token, sizeof g_token, "%s", s);
    char resp[2048];
    int st = ps_get("/api/v1/me", resp, sizeof resp);
    int authed = (st >= 200 && st < 300) &&
                 (strstr(resp, "\"authenticated\":true") || strstr(resp, "\"authenticated\": true"));
    if (!authed) {
        snprintf(g_token, sizeof g_token, "%s", old);
        if (err) snprintf(err, errcap, st < 0 ? "cannot reach the store (check the URL / network)"
                                              : "that license key is not valid");
        BSDR_WARN(TAG, "plugin-store license-key sign-in failed (HTTP %d)", st);
        return 0;
    }
    char em[256] = "";
    bsdr_json_get_str(resp, "email", em, sizeof em);
    snprintf(g_email, sizeof g_email, "%s", em[0] ? em : "license key");
    ps_save_cfg();
    BSDR_INFO(TAG, "signed in to the plugin store via license key as %s", g_email);
    return 1;
}

void bsdr_plugstore_logout(void) {
    ps_load_cfg();
    g_token[0] = '\0';
    g_email[0] = '\0';
    ps_save_cfg();
}

int bsdr_plugstore_set_url(const char *url) {
    if (!url || !url[0]) return 0;
    ps_load_cfg();
    snprintf(g_url, sizeof g_url, "%s", url);
    size_t l = strlen(g_url);
    while (l > 0 && g_url[l - 1] == '/') g_url[--l] = '\0';
    ps_save_cfg();
    return 1;
}

/* ---- catalog / purchase --------------------------------------------------------------------- */

int bsdr_plugstore_catalog_json(char *out, size_t cap, char *err, size_t errcap) {
    ps_load_cfg();
    /* Tell the store our platform/arch/ABI so each plugin reports the newest version WE can load
     * (compatible_version) — the UI compares it against what's installed to flag updates. */
    char apipath[256];
    snprintf(apipath, sizeof apipath, "/api/v1/catalog?platform=%s&arch=%s&abi=%d",
             ps_platform(), ps_arch(), BSDR_PLUGIN_ABI);
    int st = ps_get(apipath, out, cap);
    if (st < 0) { if (err) snprintf(err, errcap, "cannot reach the store"); return 0; }
    if (st != 200) { if (err) snprintf(err, errcap, "store returned HTTP %d", st); return 0; }
    return 1;
}

int bsdr_plugstore_buy_url(const char *slug, char *out, size_t cap) {
    if (!valid_slug(slug)) return 0;
    char base[512];
    base_url(base, sizeof base);
    snprintf(out, cap, "%s/buy/%s", base, slug);
    return 1;
}

/* ---- disabled-list: <config_dir>/plugins.disabled ------------------------------------------- */

static int disabled_path(char *out, size_t cap) {
    char dir[768];
    if (!bsdr_config_dir(dir, sizeof dir)) return 0;
    snprintf(out, cap, "%s/plugins.disabled", dir);
    return 1;
}

/* Match one plugin name against a line, ignoring surrounding whitespace/CR. */
static int line_is(const char *line, size_t len, const char *name) {
    while (len && (line[0] == ' ' || line[0] == '\t')) { line++; len--; }
    while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) len--;
    return len == strlen(name) && strncmp(line, name, len) == 0;
}

int bsdr_plugin_name_disabled(const char *name) {
    if (!name || !name[0]) return 0;
    char path[900];
    if (!disabled_path(path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *p = buf;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (line_is(p, l, name)) return 1;
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

/* Rewrite the disabled list so `name` is present (disable=1) or absent (disable=0). */
static int set_disabled(const char *name, int disable) {
    char path[900];
    if (!disabled_path(path, sizeof path)) return 0;
    /* read existing names (minus `name`) */
    char keep[4096]; size_t ko = 0; keep[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        const char *p = buf;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t l = eol ? (size_t)(eol - p) : strlen(p);
            /* trim trailing CR/space for storage */
            size_t t = l; while (t && (p[t-1]=='\r'||p[t-1]==' '||p[t-1]=='\t')) t--;
            size_t s = 0; while (s < t && (p[s]==' '||p[s]=='\t')) s++;
            if (t > s && !line_is(p, l, name) && ko + (t - s) + 2 < sizeof keep) {
                memcpy(keep + ko, p + s, t - s); ko += t - s; keep[ko++] = '\n'; keep[ko] = '\0';
            }
            if (!eol) break;
            p = eol + 1;
        }
    }
    f = fopen(path, "wb");
    if (!f) { BSDR_WARN(TAG, "cannot write %s", path); return 0; }
    fputs(keep, f);
    if (disable) fprintf(f, "%s\n", name);
    fclose(f);
    return 1;
}

/* ---- installed-version manifest: <config_dir>/plugins.installed (slug=version) -------------- */

static int manifest_path(char *out, size_t cap) {
    char dir[768];
    if (!bsdr_config_dir(dir, sizeof dir)) return 0;
    snprintf(out, cap, "%s/plugins.installed", dir);
    return 1;
}

/* Read the recorded installed version for slug into out (empty if unknown). */
static void manifest_get(const char *slug, char *out, size_t cap) {
    out[0] = '\0';
    char path[900];
    if (!manifest_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    cfg_field(buf, slug, out, cap);
}

/* Upsert slug=version in the manifest. */
static void manifest_set(const char *slug, const char *ver) {
    char path[900];
    if (!manifest_path(path, sizeof path)) return;
    char keep[4096]; size_t ko = 0; keep[0] = '\0';
    size_t sl = strlen(slug);
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        const char *p = buf;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t l = eol ? (size_t)(eol - p) : strlen(p);
            int same = (l > sl && strncmp(p, slug, sl) == 0 && p[sl] == '=');
            if (!same && l && ko + l + 2 < sizeof keep) { memcpy(keep + ko, p, l); ko += l; keep[ko++] = '\n'; keep[ko] = '\0'; }
            if (!eol) break;
            p = eol + 1;
        }
    }
    f = fopen(path, "wb");
    if (!f) return;
    fputs(keep, f);
    if (ver && ver[0]) fprintf(f, "%s=%s\n", slug, ver);
    fclose(f);
}

/* Ask the store which version WE would get for slug (newest compatible with our ABI/platform). */
static void fetch_compatible_version(const char *slug, char *out, size_t cap) {
    out[0] = '\0';
    char apipath[400];
    snprintf(apipath, sizeof apipath, "/api/v1/plugins/%s?platform=%s&arch=%s&abi=%d",
             slug, ps_platform(), ps_arch(), BSDR_PLUGIN_ABI);
    char resp[8192];
    if (ps_get(apipath, resp, sizeof resp) != 200) return;
    if (!bsdr_json_get_str(resp, "compatible_version", out, cap) || !out[0])
        bsdr_json_get_str(resp, "latest_version", out, cap);
}

/* ---- download / install --------------------------------------------------------------------- */

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* Pull the single shared object out of a downloaded plugin zip (which nests it under <name>/) and
 * write it flat to `dest`. Returns 1 on success. */
static int extract_so(const char *zip, const char *dest) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip, 0)) { BSDR_WARN(TAG, "cannot open %s", zip); return 0; }
    int found = -1;
    mz_uint num = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < num; i++) {
        char name[512];
        if (mz_zip_reader_get_filename(&z, i, name, sizeof name) == 0) continue;
        if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
        if (ends_with(name, PLG_EXT)) { found = (int)i; break; }
    }
    int ok = 0;
    if (found < 0) BSDR_WARN(TAG, "no %s inside %s", PLG_EXT, zip);
    else {
        char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.part", dest);
        if (mz_zip_reader_extract_to_file(&z, (mz_uint)found, tmp, 0)) {
            remove(dest);
            if (rename(tmp, dest) == 0) ok = 1; else remove(tmp);
        } else BSDR_WARN(TAG, "extract failed from %s", zip);
    }
    mz_zip_reader_end(&z);
    return ok;
}

/* Parse a flat JSON array of strings — "key":["a","b",…] — out of a small store response into
 * names[max][128]; returns how many were copied. Slugs are [a-z0-9._-] so no unescaping is needed;
 * anything with a backslash is copied verbatim (validated by the caller via valid_slug). */
static int ps_json_str_array(const char *json, const char *key, char names[][128], int max) {
    char pat[160];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '[') return 0;                 /* not an array (e.g. null) */
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p && *p != '"' && *p != ']') p++;
        if (*p != '"') break;
        p++;
        int j = 0;
        while (*p && *p != '"' && j < 127) names[n][j++] = *p++;
        names[n][j] = '\0';
        if (*p == '"') p++;
        if (names[n][0]) n++;
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    return n;
}

/* Dependencies the store records for this plugin (platform-independent slugs), into names[max]. */
static int fetch_deps(const char *slug, char names[][128], int max) {
    char apipath[400];
    snprintf(apipath, sizeof apipath, "/api/v1/plugins/%s?platform=%s&arch=%s&abi=%d",
             slug, ps_platform(), ps_arch(), BSDR_PLUGIN_ABI);
    char resp[8192];
    if (ps_get(apipath, resp, sizeof resp) != 200) return 0;
    return ps_json_str_array(resp, "deps", names, max);
}

/* Is this plugin already available to the host — either currently loaded (it may ship with the app in a
 * read-only dir) or present as a store-installed .so in the per-user dir? Used to avoid re-fetching a
 * dependency that's already satisfied. */
static int ps_present(const char *slug) {
    if (bsdr_plugins_is_loaded(slug)) return 1;
    char udir[1024];
    if (!bsdr_plugins_user_dir(udir, sizeof udir)) return 0;
    char path[1200]; snprintf(path, sizeof path, "%s/%s%s", udir, slug, PLG_EXT);
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---- download progress (web-UI) — one at a time, a singleton like model_store's g_dl ------------ */
static bsdr_mutex   *g_psdl_lock;
static bsdr_model_dl g_psdl;
static void ps_dl_lock_init(void) { if (!g_psdl_lock) g_psdl_lock = bsdr_mutex_new(); }
static void ps_dl_progress(size_t done, size_t total) {   /* bsdr_http_download byte callback */
    if (!g_psdl_lock) return;
    bsdr_mutex_lock(g_psdl_lock);
    g_psdl.done = (long)done; g_psdl.total = (long)total;
    g_psdl.pct  = total ? (int)((done * 100) / total) : -1;
    bsdr_mutex_unlock(g_psdl_lock);
}
static void ps_dl_set_current(const char *slug) {   /* the file we're fetching now (deps then target) */
    if (!g_psdl_lock) return;
    bsdr_mutex_lock(g_psdl_lock);
    snprintf(g_psdl.name, sizeof g_psdl.name, "%s", slug);
    g_psdl.done = g_psdl.total = 0; g_psdl.pct = -1;
    bsdr_mutex_unlock(g_psdl_lock);
}

/* ---- "payload" plugins: deliver files somewhere other than <plugins>/<slug>.so ------------------ *
 * A store item whose zip contains a "bsdr-payload.conf" ("target=onnx-provider") isn't a loadable
 * bsdr_plugin — it ships data files to a target dir. Used by the GPU CUDA provider "plugin", which
 * drops libonnxruntime_providers_cuda.so beside libonnxruntime.so so the ONNX plugins can use CUDA
 * without a ~700 MB base bundle. The CUDA/cuDNN runtime is host-provided; absent it, ORT can't load the
 * provider and select_ep() falls back to CPU (surfaced in the UI). */

/* Directory that holds libonnxruntime.so (where ORT looks for its provider .so's), preferring a WRITABLE
 * one. Best source: an already-loaded libonnxruntime (an ONNX plugin loads it) via dladdr; else probe
 * the dev/install layouts. Returns 1 + writes the dir. */
static int onnx_provider_dir(char *out, size_t cap) {
#if !defined(_WIN32)
    for (int pass = 0; pass < 2; pass++) {
        void *h = dlopen(pass ? "libonnxruntime.so" : "libonnxruntime.so.1", RTLD_NOLOAD | RTLD_LAZY);
        if (!h) continue;
        void *sym = dlsym(h, "OrtGetApiBase");
        Dl_info di;
        if (sym && dladdr(sym, &di) && di.dli_fname && di.dli_fname[0]) {
            char p[1024]; snprintf(p, sizeof p, "%s", di.dli_fname);
            char *s = strrchr(p, '/');
            if (s) { *s = 0; if (access(p, W_OK) == 0) { snprintf(out, cap, "%s", p); dlclose(h); return 1; } }
        }
        dlclose(h);
    }
    static const char *cands[] = { "third_party/onnxruntime/linux-x64/lib", "/opt/bsdrX/lib", "lib", NULL };
    for (int i = 0; cands[i]; i++) {
        char probe[1200]; snprintf(probe, sizeof probe, "%s/libonnxruntime.so", cands[i]);
        if (access(probe, F_OK) == 0 && access(cands[i], W_OK) == 0) { snprintf(out, cap, "%s", cands[i]); return 1; }
    }
#endif
    (void)out; (void)cap;
    return 0;
}

/* If `zip` carries a payload manifest, copy its target string ("onnx-provider") into `target`. */
static int zip_payload_target(const char *zip, char *target, size_t tcap) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip, 0)) return 0;
    int got = 0;
    int idx = mz_zip_reader_locate_file(&z, "bsdr-payload.conf", NULL, 0);
    if (idx >= 0) {
        size_t sz = 0; char *buf = (char *)mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
        if (buf) {
            char *t = strstr(buf, "target=");
            if (t) { t += 7; size_t n = 0; while (t[n] && t[n] != '\n' && t[n] != '\r' && n + 1 < tcap) { target[n] = t[n]; n++; } target[n] = 0; got = 1; }
            mz_free(buf);
        }
    }
    mz_zip_reader_end(&z);
    return got;
}

/* Extract every file in `zip` (except the manifest) into the target payload dir. Returns 1 if all landed. */
static int install_payload(const char *zip, const char *target, char *err, size_t errcap) {
    char dir[1024];
    if (strcmp(target, "onnx-provider") != 0) { if (err) snprintf(err, errcap, "unknown payload target '%s'", target); return 0; }
    if (!onnx_provider_dir(dir, sizeof dir)) {
        if (err) snprintf(err, errcap, "no writable ONNX Runtime lib dir (this build's ORT dir is read-only \xe2\x80\x94 use the GPU-bundled build)");
        return 0;
    }
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip, 0)) { if (err) snprintf(err, errcap, "cannot open package"); return 0; }
    mz_uint num = mz_zip_reader_get_num_files(&z);
    int okc = 0, total = 0;
    for (mz_uint i = 0; i < num; i++) {
        char name[512];
        if (mz_zip_reader_get_filename(&z, i, name, sizeof name) == 0) continue;
        if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
        if (strcmp(name, "bsdr-payload.conf") == 0) continue;
        const char *bn = strrchr(name, '/'); bn = bn ? bn + 1 : name;
        char dest[1600]; snprintf(dest, sizeof dest, "%s/%s", dir, bn);
        char tmp[1610]; snprintf(tmp, sizeof tmp, "%s.part", dest);
        total++;
        if (mz_zip_reader_extract_to_file(&z, i, tmp, 0)) { remove(dest); if (rename(tmp, dest) == 0) okc++; else remove(tmp); }
        else remove(tmp);
    }
    mz_zip_reader_end(&z);
    if (okc > 0 && okc == total) { BSDR_INFO(TAG, "installed payload -> %s (%d file(s))", dir, okc); return 1; }
    if (err) snprintf(err, errcap, "extracted %d/%d payload file(s)", okc, total);
    return 0;
}

/* Download + extract + record ONE plugin (no dependency handling, no reload). Returns 1 on success. */
static int ps_install_one(const char *slug, char *err, size_t errcap) {
    char udir[1024];
    if (!bsdr_plugins_user_dir(udir, sizeof udir)) { if (err) snprintf(err, errcap, "no writable plugin dir"); return 0; }

    char base[512];
    base_url(base, sizeof base);

    /* Ask for the newest build this host's ABI can load, for our platform/arch. `abi` is THIS host's
     * current ABI (a single point); the store returns the newest version whose build declares a range
     * [abi_min .. abi_max] that contains it. Auth via ?key= since the streaming downloader takes no
     * headers (the store accepts it as a query-param bearer). */
    char url[1400];
    int m = snprintf(url, sizeof url,
        "%s/api/v1/plugins/%s/download?platform=%s&arch=%s&abi=%d",
        base, slug, ps_platform(), ps_arch(), BSDR_PLUGIN_ABI);
    if (m > 0 && g_token[0] && (size_t)m < sizeof url)
        snprintf(url + m, sizeof url - m, "&key=%s", g_token);

    char zip[1200]; snprintf(zip, sizeof zip, "%s/%s.zip.part", udir, slug);
    ps_dl_set_current(slug);
    if (bsdr_http_download(url, zip, ps_dl_progress) != 0) {
        remove(zip);
        if (err) snprintf(err, errcap,
            "download failed \xe2\x80\x94 you may need to buy it first, sign in, or there's no build for %s/%s",
            ps_platform(), ps_arch());
        return 0;
    }

    /* Payload plugin? (e.g. the GPU CUDA provider) — deliver its files to a target dir, not plugins/. */
    char ptarget[64] = "";
    if (zip_payload_target(zip, ptarget, sizeof ptarget)) {
        int pok = install_payload(zip, ptarget, err, errcap);
        remove(zip);
        if (!pok) return 0;
        set_disabled(slug, 0);
        char pver[64]; fetch_compatible_version(slug, pver, sizeof pver);
        manifest_set(slug, pver[0] ? pver : "payload");
        BSDR_INFO(TAG, "installed payload plugin '%s' (target %s)", slug, ptarget);
        return 1;
    }

    char dest[1200]; snprintf(dest, sizeof dest, "%s/%s%s", udir, slug, PLG_EXT);
    int ok = extract_so(zip, dest);
    remove(zip);
    if (!ok) { if (err) snprintf(err, errcap, "downloaded file was not a valid plugin package"); return 0; }

    set_disabled(slug, 0);          /* a freshly downloaded/updated plugin is enabled */
    /* Record the version we just installed so the UI can later detect an available update. */
    char ver[64]; fetch_compatible_version(slug, ver, sizeof ver);
    manifest_set(slug, ver);
    BSDR_INFO(TAG, "installed plugin '%s'%s%s from the store", slug, ver[0] ? " v" : "", ver);
    return 1;
}

/* Install a plugin and everything it depends on: fetch its dependency slugs, install any that aren't
 * already present (depth-first, so a dependency is on disk before its dependent), then install this one.
 * `force` re-installs `slug` even if present (used for the explicitly-requested target; dependencies use
 * force=0 so an already-satisfied one is left alone). `visited` guards against cycles and diamonds. */
#define PS_DEP_MAX      12
#define PS_DEP_MAXDEPTH 12
#define PS_VISITED_MAX  48
static int ps_install_rec(const char *slug, int force, char visited[][128], int *nv, int depth,
                          char *err, size_t errcap) {
    if (!valid_slug(slug)) { if (err) snprintf(err, errcap, "bad plugin id '%s'", slug); return 0; }
    if (depth > PS_DEP_MAXDEPTH) { if (err) snprintf(err, errcap, "dependency chain too deep at '%s'", slug); return 0; }
    for (int i = 0; i < *nv; i++) if (strcmp(visited[i], slug) == 0) return 1;   /* already handled */
    if (*nv < PS_VISITED_MAX) snprintf(visited[(*nv)++], 128, "%s", slug);

    char deps[PS_DEP_MAX][128];
    int nd = fetch_deps(slug, deps, PS_DEP_MAX);
    for (int i = 0; i < nd; i++) {
        if (!valid_slug(deps[i])) continue;
        if (ps_present(deps[i])) continue;                 /* already satisfied — leave it as-is */
        char derr[256] = "";
        if (!ps_install_rec(deps[i], 0, visited, nv, depth + 1, derr, sizeof derr)) {
            if (err) snprintf(err, errcap, "requires '%s', which could not be installed (%s)",
                              deps[i], derr[0] ? derr : "unavailable");
            return 0;
        }
    }

    if (!force && ps_present(slug)) return 1;              /* a dependency that's already installed */
    return ps_install_one(slug, err, errcap);
}

int bsdr_plugstore_download(const char *slug, char *err, size_t errcap) {
    if (!valid_slug(slug)) { if (err) snprintf(err, errcap, "bad plugin id"); return 0; }
    ps_load_cfg();

    char udir[1024];
    if (!bsdr_plugins_user_dir(udir, sizeof udir)) { if (err) snprintf(err, errcap, "no writable plugin dir"); return 0; }

    /* Install the plugin and any dependencies it declares (dependencies first), then reload the whole
     * set ONCE so the host resolves load order and a plugin never comes up before something it needs. */
    char visited[PS_VISITED_MAX][128];
    int nv = 0;
    if (!ps_install_rec(slug, 1 /*force the requested plugin*/, visited, &nv, 0, err, errcap)) return 0;

    bsdr_plugins_reload();
    return 1;
}

/* Async wrapper: run bsdr_plugstore_download on a detached thread, publishing byte progress + the final
 * result into g_psdl for the web UI to poll (the sync path already updates progress via ps_install_one). */
static void ps_dl_worker(void *arg) {
    char *slug = arg;
    char err[256] = "";
    int ok = bsdr_plugstore_download(slug, err, sizeof err);
    bsdr_mutex_lock(g_psdl_lock);
    g_psdl.active = 0;
    g_psdl.ok = ok;
    if (ok) { g_psdl.pct = 100; g_psdl.err[0] = 0; }
    else snprintf(g_psdl.err, sizeof g_psdl.err, "%s", err[0] ? err : "download failed");
    bsdr_mutex_unlock(g_psdl_lock);
    free(slug);
}

int bsdr_plugstore_download_start(const char *slug) {
    if (!valid_slug(slug)) return -1;
    ps_dl_lock_init();
    if (!g_psdl_lock) return -1;
    bsdr_mutex_lock(g_psdl_lock);
    if (g_psdl.active) { bsdr_mutex_unlock(g_psdl_lock); return 0; }   /* one at a time */
    memset(&g_psdl, 0, sizeof g_psdl);
    g_psdl.active = 1; g_psdl.pct = -1;
    snprintf(g_psdl.name, sizeof g_psdl.name, "%s", slug);
    bsdr_mutex_unlock(g_psdl_lock);
    char *arg = strdup(slug);
    if (!arg || !bsdr_thread_start_detached(ps_dl_worker, arg)) {
        free(arg);
        bsdr_mutex_lock(g_psdl_lock);
        g_psdl.active = 0; snprintf(g_psdl.err, sizeof g_psdl.err, "cannot start download thread");
        bsdr_mutex_unlock(g_psdl_lock);
        return -1;
    }
    return 0;
}

void bsdr_plugstore_download_state(bsdr_model_dl *out) {
    if (!out) return;
    ps_dl_lock_init();
    if (!g_psdl_lock) { memset(out, 0, sizeof *out); return; }
    bsdr_mutex_lock(g_psdl_lock);
    *out = g_psdl;
    bsdr_mutex_unlock(g_psdl_lock);
}

/* ---- local plugin management ---------------------------------------------------------------- */

int bsdr_plugstore_set_enabled(const char *name, int on, char *err, size_t errcap) {
    if (!valid_slug(name)) { if (err) snprintf(err, errcap, "bad plugin id"); return 0; }
    if (!set_disabled(name, on ? 0 : 1)) { if (err) snprintf(err, errcap, "cannot update the disabled list"); return 0; }
    bsdr_plugins_reload();
    return 1;
}

int bsdr_plugstore_remove(const char *name, char *err, size_t errcap) {
    if (!valid_slug(name)) { if (err) snprintf(err, errcap, "bad plugin id"); return 0; }
    char udir[1024];
    if (!bsdr_plugins_user_dir(udir, sizeof udir)) { if (err) snprintf(err, errcap, "no plugin dir"); return 0; }
    char path[1200]; snprintf(path, sizeof path, "%s/%s%s", udir, name, PLG_EXT);
    struct stat st;
    if (stat(path, &st) != 0) { if (err) snprintf(err, errcap, "not a store-installed plugin (can't remove)"); return 0; }
    /* unload first so the .so isn't in use, then delete and rescan */
    bsdr_plugins_unload();
    remove(path);
    set_disabled(name, 0);          /* drop any stale disabled entry */
    manifest_set(name, "");         /* forget the recorded installed version */
    bsdr_plugins_reload();
    BSDR_INFO(TAG, "removed plugin '%s'", name);
    return 1;
}

/* ---- status --------------------------------------------------------------------------------- */

size_t bsdr_plugstore_status_json(char *out, size_t cap) {
    ps_load_cfg();
    char ue[600], ee[512];
    bsdr_json_escape(ue, sizeof ue, g_url);
    bsdr_json_escape(ee, sizeof ee, g_email);
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o,
        "{\"ok\":true,\"url\":\"%s\",\"loggedIn\":%s,\"email\":\"%s\",\"platform\":\"%s\",\"arch\":\"%s\",\"installed\":[",
        ue, g_token[0] ? "true" : "false", ee, ps_platform(), ps_arch());

    /* List shared objects in the per-user plugin dir (store-installed), with enabled/loaded flags. */
    char udir[1024];
    if (bsdr_plugins_user_dir(udir, sizeof udir)) {
        DIR *d = opendir(udir);
        if (d) {
            struct dirent *de; int emitted = 0;
            size_t extlen = strlen(PLG_EXT);
            while ((de = readdir(d)) && o < cap - 300) {
                size_t nl = strlen(de->d_name);
                if (nl <= extlen || strcmp(de->d_name + nl - extlen, PLG_EXT) != 0) continue;
                char nm[256];
                size_t base_len = nl - extlen;
                if (base_len >= sizeof nm) base_len = sizeof nm - 1;
                memcpy(nm, de->d_name, base_len); nm[base_len] = '\0';
                int enabled = !bsdr_plugin_name_disabled(nm);
                int loaded  = bsdr_plugins_is_loaded(nm);
                char ver[64]; manifest_get(nm, ver, sizeof ver);
                char ne[512]; bsdr_json_escape(ne, sizeof ne, nm);
                char ve[128]; bsdr_json_escape(ve, sizeof ve, ver);
                o += (size_t)snprintf(out + o, cap - o,
                    "%s{\"name\":\"%s\",\"enabled\":%s,\"loaded\":%s,\"version\":\"%s\"}",
                    emitted ? "," : "", ne, enabled ? "true" : "false", loaded ? "true" : "false", ve);
                emitted++;
            }
            closedir(d);
        }
    }
    o += (size_t)snprintf(out + o, cap - o, "]}");
    return o;
}
