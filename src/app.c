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
#include "bsdr/app.h"
#include "bsdr/cloud.h"
#include "bsdr/cloud_stream.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/model_store.h"
#include "bsdr/tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp (portable; bionic/musl don't expose it via string.h) */
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#  include <direct.h>          /* _mkdir */
#  define bsdr_mkdir(p)  _mkdir(p)
#else
#  define bsdr_mkdir(p)  mkdir((p), 0700)
#endif

/* ---- playlist helpers ---------------------------------------------------- */
bool bsdr_path_is_playlist(const char *src) {
    if (!src) return false;
    size_t n = strlen(src);
    if (n < 4) return false;
    const char *e = src + n - 4;   /* case-insensitive ".txt" */
    return e[0] == '.' && (e[1]|0x20) == 't' && (e[2]|0x20) == 'x' && (e[3]|0x20) == 't';
}

/* trim leading/trailing whitespace (incl. CR/LF) in place; returns the start. */
static char *pl_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

/* Network sources the operator may stream (mirrors the avformat protocol whitelist below). Only
 * these URL schemes are accepted; everything else (file://, concat:, gopher:, rtmp:, …) is refused
 * so a crafted entry can't reach arbitrary protocols. Case-insensitive, must be followed by "://". */
bool bsdr_url_scheme_ok(const char *s) {
    static const char *ok[] = { "http", "https", "rtsp", NULL };
    for (int i = 0; ok[i]; i++) {
        size_t l = strlen(ok[i]);
        if (strncasecmp(s, ok[i], l) == 0 && strncmp(s + l, "://", 3) == 0) return true;
    }
    return false;
}

/* A playlist file is UNTRUSTED. Accept a plausible local filesystem path OR an allowlisted network
 * URL (http/https/rtsp — the operator explicitly asked to stream these): no other URL scheme, no
 * pipe/concat list separator, no embedded control chars, not over-long. Defense in depth on top of
 * the avformat protocol whitelist at open time (capture.c / fileaudio.c). */
static bool pl_path_ok(const char *s) {
    if (!*s || *s == '#') return false;
    size_t n = strlen(s);
    if (n >= 512) return false;
    if (strchr(s, '|')) return false;     /* concat/pipe list */
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x20) return false;  /* control chars */
    if (strstr(s, "://")) return bsdr_url_scheme_ok(s);   /* only http/https/rtsp URLs */
    return true;
}

#define BSDR_PLAYLIST_MAX 8192   /* cap so a huge/hostile .txt can't stall the parse */

int bsdr_playlist_entry(const char *src, int i, char *out, size_t outlen) {
    if (!src || !*src || !out || outlen == 0) return 0;
    if (!bsdr_path_is_playlist(src)) { snprintf(out, outlen, "%s", src); return 1; }
    FILE *f = fopen(src, "r");
    if (!f) return 0;
    char line[1024];   /* wider than a valid entry so over-long lines are seen (and rejected) whole */
    int n = 0;
    while (n < BSDR_PLAYLIST_MAX && fgets(line, sizeof line, f)) { char *s = pl_trim(line); if (pl_path_ok(s)) n++; }
    if (n == 0) { fclose(f); return 0; }
    int idx = ((i % n) + n) % n, k = 0;
    char chosen[512] = "";
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        char *s = pl_trim(line);
        if (!pl_path_ok(s)) continue;
        if (k++ == idx) { snprintf(chosen, sizeof chosen, "%s", s); break; }
    }
    fclose(f);
    if (!chosen[0]) return 0;
    /* a URL (http/https/rtsp) passes through verbatim — never treated as a relative path */
    if (strstr(chosen, "://")) {
        snprintf(out, outlen, "%s", chosen);
    } else
    /* absolute (/... or drive-letter C:...) stays; relative resolves against the .txt's dir */
    if (chosen[0] == '/' || (chosen[0] && chosen[1] == ':')) {
        snprintf(out, outlen, "%s", chosen);
    } else {
        const char *sl = strrchr(src, '/');
        if (sl) snprintf(out, outlen, "%.*s/%s", (int)(sl - src), src, chosen);
        else    snprintf(out, outlen, "%s", chosen);
    }
    return n;
}

void bsdr_app_init(bsdr_app *a) {
    memset(a, 0, sizeof(*a));
    a->lock = bsdr_mutex_new();
    snprintf(a->source, sizeof(a->source), "desktop");
    a->file_volume = 100;   /* media bar default */
    a->threed_deepness = 35; a->threed_convergence = 0;   /* comfortable defaults when 3D is enabled */
    a->threed_full = 0;      /* default: light half-SBS (full-res is ~4x the load; opt-in) */
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "not logged in");
    a->cloud_auto_share = true;   /* follow the Quest's RDC screen (auto start/stop sharing) */
    /* Cloud VIDEO is ON: the relay video is plain H.264 (NOT encrypted, as first thought) but uses
     * Bigscreen's CUSTOM raw fragmentation (not FU-A) — reversed from full.pcapng. bsdr_video_send_au_cloud
     * matches it. Cloud AUDIO is plain Opus RTP (pt 100, djb2 SSRC, ts+=480) + the 8-byte BigSoup
     * trailer ([u32 ssrc LE][u32 frame_id LE], no XOR) so the Quest reads our SSRC instead of 0.
     * Confirmed working on a live Quest; ON by default like video — disable with --no-cloud-audio. */
    a->cloud_no_video = false;
    a->video_decoupled = false;    /* default: couple cloud to the single LAN encoder */
    a->cpu_only = false;           /* default: try the CUDA GPU capture pipeline */
    a->use_vaapi = false;          /* default off: opt-in VAAPI iGPU encode */
    a->use_kmsgrab = false;        /* default off: opt-in DRM/KMS capture */
    a->cloud_no_audio = false;
    a->audio = true;
    a->bitrate = 8000000;     /* 8 Mbps default */
    a->res_w = 0;             /* width auto-derived from the desktop aspect ratio */
    a->res_h = 720;           /* 720p default; the headset overrides via PUT /device */
}

void bsdr_app_set_quality(bsdr_app *a, int w, int h, int bitrate) {
    bsdr_mutex_lock(a->lock);
    if (w >= 0) a->res_w = w;
    if (h >= 0) a->res_h = h;
    /* The headset's bitrate is a fixed cycle (1/3/5/8/.../100 Mbps) and it forces 3 Mbps when
     * internet sharing turns on. --max-bitrate lets the operator cap it below whatever the Quest
     * sends (e.g. hold 1 Mbps for a constrained uplink). */
    if (bitrate > 0) {
        if (a->max_bitrate > 0 && bitrate > a->max_bitrate) bitrate = a->max_bitrate;
        a->bitrate = bitrate;
    }
    bsdr_mutex_unlock(a->lock);
    char wbuf[16];
    if (a->res_w > 0) snprintf(wbuf, sizeof(wbuf), "%d", a->res_w);
    else              snprintf(wbuf, sizeof(wbuf), "auto");   /* width from desktop aspect */
    BSDR_INFO("bsdr.app", "video quality: %sx%d @ %d bps (auto width = derived from desktop aspect)",
              wbuf, a->res_h, a->bitrate);
}

void bsdr_app_get_quality(bsdr_app *a, int *w, int *h, int *bitrate) {
    bsdr_mutex_lock(a->lock);
    if (w) *w = a->res_w;
    if (h) *h = a->res_h;
    if (bitrate) *bitrate = a->bitrate;
    bsdr_mutex_unlock(a->lock);
}
void bsdr_app_set_region(bsdr_app *a, int x, int y, int w, int h) {
    bsdr_mutex_lock(a->lock);
    a->cap_x = x; a->cap_y = y; a->cap_w = w; a->cap_h = h;
    bsdr_mutex_unlock(a->lock);
}
void bsdr_app_get_region(bsdr_app *a, int *x, int *y, int *w, int *h) {
    bsdr_mutex_lock(a->lock);
    if (x) *x = a->cap_x;
    if (y) *y = a->cap_y;
    if (w) *w = a->cap_w;
    if (h) *h = a->cap_h;
    bsdr_mutex_unlock(a->lock);
}

static void setif(char *dst, size_t n, const char *src) {
    if (src && *src) snprintf(dst, n, "%s", src);
}
void bsdr_app_set_voice(bsdr_app *a, const char *se, const char *st, const char *sm,
                        const char *le, const char *lt, const char *lm) {
    bsdr_mutex_lock(a->lock);
    setif(a->stt_endpoint, sizeof(a->stt_endpoint), se);
    setif(a->stt_token, sizeof(a->stt_token), st);
    setif(a->stt_model, sizeof(a->stt_model), sm);
    setif(a->llm_endpoint, sizeof(a->llm_endpoint), le);
    setif(a->llm_token, sizeof(a->llm_token), lt);
    setif(a->llm_model, sizeof(a->llm_model), lm);
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "voice config: stt=%s llm=%s", a->stt_endpoint, a->llm_endpoint);
}
void bsdr_app_get_voice(bsdr_app *a, char *se, char *st, char *sm,
                        char *le, char *lt, char *lm, size_t each) {
    bsdr_mutex_lock(a->lock);
    snprintf(se, each, "%s", a->stt_endpoint); snprintf(st, each, "%s", a->stt_token);
    snprintf(sm, each, "%s", a->stt_model);    snprintf(le, each, "%s", a->llm_endpoint);
    snprintf(lt, each, "%s", a->llm_token);    snprintf(lm, each, "%s", a->llm_model);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_compctl(bsdr_app *a, bool want, bool vision) {
    bsdr_mutex_lock(a->lock);
    a->compctl_want = want;
    a->compctl_vision = vision;
    a->compctl_dirty = true;
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "computer control: %s%s", want ? "requested" : "off",
              vision ? " (vision)" : "");
}
bool bsdr_app_take_compctl(bsdr_app *a, bool *want, bool *vision) {
    bsdr_mutex_lock(a->lock);
    bool dirty = a->compctl_dirty;
    if (dirty) { *want = a->compctl_want; *vision = a->compctl_vision; a->compctl_dirty = false; }
    bsdr_mutex_unlock(a->lock);
    return dirty;
}
void bsdr_app_set_compctl_status(bsdr_app *a, bool active, const char *msg) {
    bsdr_mutex_lock(a->lock);
    a->compctl_active = active;
    if (msg) snprintf(a->compctl_msg, sizeof(a->compctl_msg), "%s", msg);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_free(bsdr_app *a) {
    if (a->cloud_stream) { bsdr_cloud_stream_stop(a->cloud_stream); a->cloud_stream = NULL; }
    if (a->lock) bsdr_mutex_free(a->lock);
}

void bsdr_app_register_quest(bsdr_app *a, const char *ip) {
    bsdr_mutex_lock(a->lock);
    uint64_t now = bsdr_now_ms();
    for (int i = 0; i < a->quest_count; i++) {
        if (strcmp(a->quests[i].ip, ip) == 0) {
            a->quests[i].last_seen_ms = now;
            bsdr_mutex_unlock(a->lock);
            return;
        }
    }
    if (a->quest_count < BSDR_MAX_QUESTS) {
        bsdr_quest_entry *q = &a->quests[a->quest_count++];
        snprintf(q->ip, sizeof(q->ip), "%s", ip);
        snprintf(q->name, sizeof(q->name), "Quest @ %s", ip);
        q->last_seen_ms = now;
        BSDR_INFO("bsdr.app", "discovered Quest %s (%d total)", ip, a->quest_count);
    }
    bsdr_mutex_unlock(a->lock);
}

bool bsdr_app_quest_allowed(bsdr_app *a, const char *ip) {
    bsdr_mutex_lock(a->lock);
    bool blocked = a->blocked_quest_ip[0] != '\0' &&
                   strcmp(a->blocked_quest_ip, ip) == 0;
    bool ok = !blocked &&
              (a->selected_quest_ip[0] == '\0' || strcmp(a->selected_quest_ip, ip) == 0);
    bsdr_mutex_unlock(a->lock);
    return ok;
}

void bsdr_app_block_quest(bsdr_app *a, const char *ip) {
    bsdr_mutex_lock(a->lock);
    snprintf(a->blocked_quest_ip, sizeof(a->blocked_quest_ip), "%s", ip ? ip : "");
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "ignoring Quest %s until reselected", ip ? ip : "");
}

void bsdr_app_set_paired(bsdr_app *a, bool paired, const char *name, const char *ip) {
    bsdr_cloud_stream *to_stop = NULL;
    bsdr_mutex_lock(a->lock);
    a->quest_paired = paired;
    if (paired) {
        snprintf(a->quest_name, sizeof(a->quest_name), "%s", name ? name : "");
        snprintf(a->quest_ip, sizeof(a->quest_ip), "%s", ip ? ip : "");
    } else {
        a->quest_name[0] = a->quest_ip[0] = '\0';
        a->streaming = false;
        a->internet_sharing = false;
        /* unpair: the session is gone — fully tear down the kept-alive relay connection */
        to_stop = a->cloud_stream; a->cloud_stream = NULL;
    }
    bsdr_mutex_unlock(a->lock);
    if (to_stop) bsdr_cloud_stream_stop(to_stop);
}

void bsdr_app_set_streaming(bsdr_app *a, bool streaming) {
    bsdr_mutex_lock(a->lock);
    a->streaming = streaming;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_blank(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->blank_want = on;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_threed(bsdr_app *a, int mode, int deepness, int convergence, int swap, int full,
                         int tier, const char *ai_cmd) {
    if (mode < 0) mode = 0; else if (mode > 2) mode = 2;
    if (deepness < 0) deepness = 0; else if (deepness > 100) deepness = 100;
    if (convergence < -50) convergence = -50; else if (convergence > 50) convergence = 50;
    if (tier < 0) tier = 0; else if (tier > 3) tier = 3;
    bsdr_mutex_lock(a->lock);
    a->threed_mode = mode;
    a->threed_deepness = deepness;
    a->threed_convergence = convergence;
    a->threed_swap = swap ? 1 : 0;
    a->threed_full = full ? 1 : 0;
    a->threed_tier = tier;
    if (ai_cmd) snprintf(a->threed_ai_cmd, sizeof(a->threed_ai_cmd), "%s", ai_cmd);
    a->threed_gen++;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_get_threed(bsdr_app *a, int *mode, int *deepness, int *convergence, int *swap,
                         int *full, int *tier, char *ai_cmd, size_t ai_len) {
    bsdr_mutex_lock(a->lock);
    if (mode) *mode = a->threed_mode;
    if (deepness) *deepness = a->threed_deepness;
    if (convergence) *convergence = a->threed_convergence;
    if (swap) *swap = a->threed_swap;
    if (full) *full = a->threed_full;
    if (tier) *tier = a->threed_tier;
    if (ai_cmd && ai_len) snprintf(ai_cmd, ai_len, "%s", a->threed_ai_cmd);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_cloud_mic_fallback(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->cloud_mic_fallback = on;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_owner_mic_local(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->owner_mic_local = on;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_relay_port(bsdr_app *a, int port) {
    bsdr_mutex_lock(a->lock);
    a->sniff_remote_port = (port > 0 && port < 65536) ? port : 0;
    bsdr_mutex_unlock(a->lock);
}
int bsdr_app_get_relay_port(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    int p = a->sniff_remote_port;
    bsdr_mutex_unlock(a->lock);
    return p;
}

static int clamp100(int v, int lo) { if (v < lo) return lo; if (v > 100) return 100; return v; }

void bsdr_app_set_voicefx(bsdr_app *a, int gender, int robot, int echo, int whisper, bool substitute) {
    bsdr_mutex_lock(a->lock);
    a->voice_gender = clamp100(gender, -100);
    a->voice_robot = clamp100(robot, 0);
    a->voice_echo = clamp100(echo, 0);
    a->voice_whisper = clamp100(whisper, 0);
    a->voice_substitute = substitute;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_get_voicefx(bsdr_app *a, int *gender, int *robot, int *echo, int *whisper, bool *substitute) {
    bsdr_mutex_lock(a->lock);
    if (gender) *gender = a->voice_gender;
    if (robot) *robot = a->voice_robot;
    if (echo) *echo = a->voice_echo;
    if (whisper) *whisper = a->voice_whisper;
    if (substitute) *substitute = a->voice_substitute;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_faceswap(bsdr_app *a, bool on, int tier, const char *source) {
    bsdr_mutex_lock(a->lock);
    a->faceswap_on = on;
    a->faceswap_tier = tier;
    if (source) snprintf(a->faceswap_source, sizeof a->faceswap_source, "%s", source);
    a->faceswap_gen++;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_get_faceswap(bsdr_app *a, bool *on, int *tier, char *source, size_t sl) {
    bsdr_mutex_lock(a->lock);
    if (on) *on = a->faceswap_on;
    if (tier) *tier = a->faceswap_tier;
    if (source) snprintf(source, sl, "%s", a->faceswap_source);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_faceswap_status(bsdr_app *a, const char *status) {
    bsdr_mutex_lock(a->lock);
    snprintf(a->faceswap_status, sizeof a->faceswap_status, "%s", status ? status : "");
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_room_pcm_sink(bsdr_app *a, void (*cb)(void *, const int16_t *, int, int), void *user) {
    a->room_pcm_user = user;   /* set user before cb so the cloud thread never sees a stale pair */
    a->room_pcm_cb = cb;
}

/* ---- session persistence (so a restart is already logged in) -------------- */
/* Path: $XDG_CONFIG_HOME/bsdr_agent/session  (or ~/.config/bsdr_agent/session), mode 0600.
 * Stores only tokens + display info — never the password. */
static bool session_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
#if defined(_WIN32)
    if (!home || !home[0]) home = getenv("APPDATA");   /* Windows: no HOME by default */
#endif
    char dir[512];
    if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/bsdr_agent", xdg);
    else if (home && home[0]) snprintf(dir, sizeof(dir), "%s/.config/bsdr_agent", home);
    else return false;
    bsdr_mkdir(dir);   /* best-effort; ignore EEXIST */
    snprintf(out, cap, "%s/session", dir);
    return true;
}

/* lines: access_token / refresh_token / email / name (token strings have no newlines). */
static void session_save(bsdr_app *a) {
    char path[600];
    if (!session_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "w");
    if (!f) { BSDR_WARN("bsdr.app", "could not save session to %s", path); return; }
#if !defined(_WIN32)
    fchmod(fileno(f), 0600);   /* tokens are user-private; NTFS ACLs handle this on Windows */
#endif
    fprintf(f, "%s\n%s\n%s\n%s\n", a->access_token, a->refresh_token, a->cloud_email, a->cloud_name);
    fclose(f);
    BSDR_DEBUG("bsdr.app", "cloud session saved to %s", path);
}

static void session_clear(void) {
    char path[600];
    if (session_path(path, sizeof(path))) remove(path);
}

void bsdr_app_login(bsdr_app *a, const char *email, const char *password) {
    bsdr_mutex_lock(a->lock);
    snprintf(a->cloud_email, sizeof(a->cloud_email), "%s", email);
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "logging in...");
    bsdr_mutex_unlock(a->lock);

    bsdr_cloud_result res;
    bool ok = bsdr_cloud_login(email, password, &res);   /* blocking HTTPS */
    char name[128] = "";
    if (ok) bsdr_cloud_account(res.access_token, name, sizeof(name));

    bsdr_mutex_lock(a->lock);
    a->cloud_logged_in = ok;
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "%.150s", res.message);
    if (ok) {
        snprintf(a->access_token, sizeof(a->access_token), "%s", res.access_token);
        snprintf(a->refresh_token, sizeof(a->refresh_token), "%s", res.refresh_token);
        snprintf(a->cloud_name, sizeof(a->cloud_name), "%s", name[0] ? name : email);
    }
    bsdr_mutex_unlock(a->lock);

    /* Open the presence WS so the host shows online and a Quest can add a screen. */
    if (ok) {
        session_save(a);   /* persist tokens so the next restart is already logged in */
        bsdr_cloud_ws *ws = bsdr_cloud_ws_open(res.access_token);
        bsdr_mutex_lock(a->lock);
        a->cloud_ws = ws;
        bsdr_mutex_unlock(a->lock);
        if (!ws) BSDR_WARN("bsdr.app", "cloud presence WS did not connect (login OK)");
    }
}

/* Restore a persisted session at startup: validate the saved access token (renew via the refresh
 * token if it's expired), set logged-in, and open the presence WS. Returns true if logged in. */
bool bsdr_app_restore_session(bsdr_app *a) {
    char path[600];
    if (!session_path(path, sizeof(path))) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char access[2048] = "", refresh[2048] = "", email[128] = "", name[128] = "";
    if (!fgets(access, sizeof(access), f))  { fclose(f); return false; }
    if (!fgets(refresh, sizeof(refresh), f)) refresh[0] = 0;
    if (!fgets(email, sizeof(email), f))     email[0] = 0;
    if (!fgets(name, sizeof(name), f))       name[0] = 0;
    fclose(f);
    /* strip trailing newlines */
    access[strcspn(access, "\r\n")] = 0; refresh[strcspn(refresh, "\r\n")] = 0;
    email[strcspn(email, "\r\n")] = 0;   name[strcspn(name, "\r\n")] = 0;
    if (!access[0]) return false;

    char vname[128] = "";
    bool valid = bsdr_cloud_account(access, vname, sizeof(vname));
    if (!valid && refresh[0]) {
        BSDR_INFO("bsdr.app", "saved token expired — renewing");
        bsdr_cloud_result rr;
        if (bsdr_cloud_renew(refresh, &rr)) {
            snprintf(access, sizeof(access), "%s", rr.access_token);
            if (rr.refresh_token[0]) snprintf(refresh, sizeof(refresh), "%s", rr.refresh_token);
            valid = bsdr_cloud_account(access, vname, sizeof(vname));
        }
    }
    if (!valid) { BSDR_INFO("bsdr.app", "saved session invalid; please log in again"); return false; }

    bsdr_mutex_lock(a->lock);
    a->cloud_logged_in = true;
    snprintf(a->access_token, sizeof(a->access_token), "%s", access);
    snprintf(a->refresh_token, sizeof(a->refresh_token), "%s", refresh);
    snprintf(a->cloud_email, sizeof(a->cloud_email), "%s", email);
    snprintf(a->cloud_name, sizeof(a->cloud_name), "%s", vname[0] ? vname : (name[0] ? name : email));
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "logged in (restored)");
    bsdr_mutex_unlock(a->lock);

    session_save(a);   /* persist any renewed token */
    bsdr_cloud_ws *ws = bsdr_cloud_ws_open(a->access_token);
    bsdr_mutex_lock(a->lock);
    a->cloud_ws = ws;
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "restored Bigscreen session for %s", a->cloud_name);
    return true;
}

void bsdr_app_logout(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bsdr_cloud_ws *ws = a->cloud_ws;
    a->cloud_ws = NULL;
    a->internet_sharing = false;
    a->cloud_logged_in = false;
    a->access_token[0] = '\0';
    a->refresh_token[0] = '\0';
    a->cloud_name[0] = '\0';
    a->cloud_email[0] = '\0';
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "not logged in");
    bsdr_mutex_unlock(a->lock);
    session_clear();                     /* forget the persisted session */
    if (ws) bsdr_cloud_ws_close(ws);     /* outside the lock: joins the keepalive thread */
    BSDR_INFO("bsdr.app", "logged out of Bigscreen account");
}

/* Renew the cloud access token using the stored refresh token (the access token is short-lived;
 * mid-session it expires and /rooms starts returning 401/403). Updates + persists the new token. */
static bool app_renew_token(bsdr_app *a) {
    char refresh[2048];
    bsdr_mutex_lock(a->lock);
    snprintf(refresh, sizeof(refresh), "%s", a->refresh_token);
    bsdr_mutex_unlock(a->lock);
    if (!refresh[0]) return false;
    bsdr_cloud_result rr;
    if (!bsdr_cloud_renew(refresh, &rr)) { BSDR_WARN("bsdr.app", "cloud token renew failed"); return false; }
    bsdr_mutex_lock(a->lock);
    snprintf(a->access_token, sizeof(a->access_token), "%s", rr.access_token);
    if (rr.refresh_token[0]) snprintf(a->refresh_token, sizeof(a->refresh_token), "%s", rr.refresh_token);
    bsdr_mutex_unlock(a->lock);
    session_save(a);
    BSDR_INFO("bsdr.app", "cloud token renewed (was expired)");
    return true;
}

/* Reconcile internet sharing: DESIRED state = a->internet_sharing (set by the Quest's
 * start/stop callbacks or the web toggle). ACTUAL state = a->cloud_stream. Start the relay
 * stream when desired ON + a screen is available; stop it when desired OFF. Runs on the agent
 * tick (the only place that may block on GET /rooms / stream start/stop). The Quest drives the
 * desired flag via cb_start/cb_stop — we do NOT poll /rooms to infer it (the screen persists in
 * the room whether or not the Quest is actively sharing, which only caused start/stop flapping). */
static void cloud_try_start(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bool logged = a->cloud_logged_in;
    bool want = a->internet_sharing;
    bsdr_cloud_stream *cs = a->cloud_stream;   /* borrow; only stop/free after claiming under lock */
    bool aud = a->audio;
    char token[2048];
    snprintf(token, sizeof(token), "%s", a->access_token);
    bsdr_mutex_unlock(a->lock);

    /* Desired OFF while we have a stream: PAUSE it — keep the sockets + SCTP association alive
     * across the toggle (like the official client, which never closes its connections; stop-
     * sharing just flips a flag). This avoids the re-INIT collision / comedia re-latch that made
     * the relay go silent on restart. Full teardown happens only on unpair / shutdown. */
    if (!want) {
        if (cs) bsdr_cloud_stream_set_active(cs, 0);
        return;
    }
    if (!logged) return;

    /* Desired ON and we already have a stream: if it's paused, decide resume vs restart by
     * re-reading /rooms — same relay tuple → just resume; changed → drop it (next tick restarts). */
    if (cs) {
        if (bsdr_cloud_stream_active(cs)) return;          /* already actively streaming */
        bsdr_cloud_screen rs;
        bool okr = bsdr_cloud_get_rooms(token, &rs) && rs.found && rs.video_port > 0 && rs.session_id[0];
        if (okr && bsdr_cloud_stream_matches(cs, &rs)) {
            bsdr_cloud_stream_set_active(cs, 1);           /* RESUME — same relay, connection still up */
            return;
        }
        if (!okr) return;                                  /* transient /rooms miss: keep paused, retry */
        /* a valid but DIFFERENT relay/session -> drop the old stream (next tick starts fresh) */
        bsdr_mutex_lock(a->lock);
        bsdr_cloud_stream *to_stop = a->cloud_stream; a->cloud_stream = NULL;
        bsdr_mutex_unlock(a->lock);
        if (to_stop) {
            BSDR_INFO("bsdr.app", "internet sharing: relay/session changed -> restarting stream");
            bsdr_cloud_stream_stop(to_stop);
        }
        return;                                            /* next tick starts fresh */
    }

    /* Desired ON, no stream: claim the start slot (one starter only) and start fresh. */
    bsdr_mutex_lock(a->lock);
    bool claim = (a->internet_sharing && !a->cloud_stream && !a->cloud_starting);
    if (claim) a->cloud_starting = true;
    snprintf(token, sizeof(token), "%s", a->access_token);
    bsdr_mutex_unlock(a->lock);
    if (!claim) return;

    bsdr_cloud_screen scr;
    bool ok = bsdr_cloud_get_rooms(token, &scr) && scr.found;
    /* token expired mid-session → /rooms 401/403. Renew with the refresh token and retry once. */
    if (!ok && (scr.http_status == 401 || scr.http_status == 403) && app_renew_token(a)) {
        bsdr_mutex_lock(a->lock);
        snprintf(token, sizeof(token), "%s", a->access_token);
        bsdr_mutex_unlock(a->lock);
        ok = bsdr_cloud_get_rooms(token, &scr) && scr.found;
    }
    /* Guard against a half-published / stale screen: without a usable video port and a
     * userSessionId the SSRC (djb2(sessionId)) and comedia target are wrong and the SFU
     * silently drops the whole session. Treat as "not ready" so the next tick re-reads. */
    if (ok && (scr.video_port <= 0 || !scr.session_id[0])) {
        BSDR_WARN("bsdr.app", "internet sharing: screen not fully published yet "
                  "(v=%d session=%.12s) — retrying", scr.video_port, scr.session_id);
        ok = false;
    }
    if (ok) {
        BSDR_INFO("bsdr.app", "internet sharing ON -> relay %s v=%d a=%d d=%d session=%s (starting stream)",
                  scr.media_ip, scr.video_port, scr.audio_port, scr.data_port, scr.session_id);
        bsdr_cloud_stream *cstr = bsdr_cloud_stream_start(&scr, a, aud);
        bsdr_mutex_lock(a->lock);
        a->cloud_starting = false;
        if (!a->cloud_stream && a->internet_sharing) {   /* still wanted → adopt it */
            a->cloud_stream = cstr;
            bsdr_mutex_unlock(a->lock);
        } else {                                         /* turned off meanwhile → discard */
            bsdr_mutex_unlock(a->lock);
            if (cstr) bsdr_cloud_stream_stop(cstr);
        }
    } else {
        /* no shareable screen yet — release the slot; the next tick retries while still wanted */
        bsdr_mutex_lock(a->lock); a->cloud_starting = false; bsdr_mutex_unlock(a->lock);
    }
}

void bsdr_app_cloud_tick(bsdr_app *a) {
    cloud_try_start(a);   /* start/stop the relay stream to match the desired (Quest-driven) flag */
}

/* The LAN encoder hands each encoded access unit here; if a relay stream is live we forward it
 * as plain RTP. Held under a->lock so the relay stream can't be freed mid-feed. ONE encoder
 * feeds both the Quest (LAN) and the relay — no second nvenc. */
void bsdr_app_feed_cloud_video(bsdr_app *a, const uint8_t *au, size_t len, int w, int h) {
#ifdef BSDR_HAVE_CAPTURE
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    if (a->cloud_stream) bsdr_cloud_stream_feed_video(a->cloud_stream, au, len, w, h);
    bsdr_mutex_unlock(a->lock);
#else
    (void)a; (void)au; (void)len; (void)w; (void)h;
#endif
}

/* Set the DESIRED sharing state only — non-blocking, so it is safe to call from the control
 * thread (the Quest's start/stop callbacks). The agent tick reconciles it into an actual relay
 * stream (start when ON + a screen is available, stop when OFF). */
void bsdr_app_set_internet_sharing(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    bool logged = a->cloud_logged_in;
    bool changed = (a->internet_sharing != on);
    a->internet_sharing = on;
    bsdr_mutex_unlock(a->lock);
    if (on && !logged) {
        BSDR_WARN("bsdr.app", "internet sharing requested but not logged in");
        return;
    }
    if (changed)
        BSDR_INFO("bsdr.app", "internet sharing %s requested", on ? "ON" : "OFF");
}

bool bsdr_app_get_internet_sharing(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bool v = a->internet_sharing;
    bsdr_mutex_unlock(a->lock);
    return v;
}

/* bumped whenever the operator picks a different headset in the UI, so the agent switches the stream */
unsigned bsdr_app_select_gen(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    unsigned g = a->select_gen;
    bsdr_mutex_unlock(a->lock);
    return g;
}

void bsdr_app_select_quest(bsdr_app *a, const char *ip) {
    bsdr_mutex_lock(a->lock);
    int changed = strcmp(a->selected_quest_ip, ip ? ip : "") != 0;
    snprintf(a->selected_quest_ip, sizeof(a->selected_quest_ip), "%s", ip ? ip : "");
    /* reselecting a blocked Quest lifts the operator-disconnect block */
    if (ip && ip[0] && strcmp(a->blocked_quest_ip, ip) == 0)
        a->blocked_quest_ip[0] = '\0';
    if (changed) a->select_gen++;   /* agent switches the active stream to the new headset */
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "selected Quest: %s", ip && ip[0] ? ip : "(any)");
}

void bsdr_app_set_source(bsdr_app *a, const char *mode, const char *path) {
    bsdr_mutex_lock(a->lock);
    if (mode) snprintf(a->source, sizeof(a->source), "%s", mode);
    if (path) snprintf(a->source_path, sizeof(a->source_path), "%s", path);
    a->source_gen++;   /* live session reopens capture with the new source */
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_source_right(bsdr_app *a, const char *dev) {
    bsdr_mutex_lock(a->lock);
    if (dev) snprintf(a->source_path2, sizeof(a->source_path2), "%s", dev);
    a->source_gen++;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_get_source_right(bsdr_app *a, char *dev, size_t dl) {
    bsdr_mutex_lock(a->lock);
    if (dev) snprintf(dev, dl, "%s", a->source_path2);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_paused(bsdr_app *a, bool paused) {
    bsdr_mutex_lock(a->lock);
    a->paused = paused;
    bsdr_mutex_unlock(a->lock);
}
bool bsdr_app_is_paused(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bool p = a->paused;
    bsdr_mutex_unlock(a->lock);
    return p;
}

void bsdr_app_request_disconnect(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    a->disconnect_req = true;
    bsdr_mutex_unlock(a->lock);
}
bool bsdr_app_take_disconnect(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bool req = a->disconnect_req;
    a->disconnect_req = false;
    bsdr_mutex_unlock(a->lock);
    return req;
}

void bsdr_app_set_sniff(bsdr_app *a, bool want, bool mitm, const char *password) {
    bsdr_mutex_lock(a->lock);
    a->sniff_want = want;
    a->sniff_mitm = mitm;
    a->sniff_dirty = true;
    if (password) snprintf(a->sniff_password, sizeof a->sniff_password, "%s", password);
    else a->sniff_password[0] = 0;
    bsdr_mutex_unlock(a->lock);
}
bool bsdr_app_take_sniff(bsdr_app *a, bool *want, bool *mitm, char *pw, size_t pwlen) {
    bsdr_mutex_lock(a->lock);
    bool dirty = a->sniff_dirty;
    if (dirty) {
        a->sniff_dirty = false;
        if (want) *want = a->sniff_want;
        if (mitm) *mitm = a->sniff_mitm;
        if (pw && pwlen) snprintf(pw, pwlen, "%s", a->sniff_password);
        memset(a->sniff_password, 0, sizeof a->sniff_password);   /* don't retain the password */
    }
    bsdr_mutex_unlock(a->lock);
    return dirty;
}
void bsdr_app_set_sniff_status(bsdr_app *a, bool active, const char *msg) {
    bsdr_mutex_lock(a->lock);
    a->sniff_active = active;
    if (msg) snprintf(a->sniff_msg, sizeof a->sniff_msg, "%s", msg);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_get_source(bsdr_app *a, char *mode, size_t ml, char *path, size_t pl) {
    bsdr_mutex_lock(a->lock);
    if (mode) snprintf(mode, ml, "%s", a->source);
    if (path) snprintf(path, pl, "%s", a->source_path);
    bsdr_mutex_unlock(a->lock);
}

size_t bsdr_app_status_json(bsdr_app *a, char *out, size_t cap) {
    bsdr_mutex_lock(a->lock);
    char esc_name[160], esc_email[160], esc_msg[200], esc_sniff[200], esc_cc[200], esc_ai[400];
    bsdr_json_escape(esc_name, sizeof(esc_name), a->cloud_name);
    bsdr_json_escape(esc_email, sizeof(esc_email), a->cloud_email);
    bsdr_json_escape(esc_msg, sizeof(esc_msg), a->cloud_msg);
    bsdr_json_escape(esc_sniff, sizeof(esc_sniff), a->sniff_msg);
    bsdr_json_escape(esc_cc, sizeof(esc_cc), a->compctl_msg);
    bsdr_json_escape(esc_ai, sizeof(esc_ai), a->threed_ai_cmd);
    int n = snprintf(out, cap,
        "{\"cloud\":{\"loggedIn\":%s,\"email\":\"%s\",\"name\":\"%s\",\"msg\":\"%s\","
        "\"internetSharing\":%s},"
        "\"quest\":{\"paired\":%s,\"name\":\"%s\",\"ip\":\"%s\",\"streaming\":%s,\"paused\":%s},"
        "\"source\":{\"mode\":\"%s\",\"path\":\"%s\",\"path2\":\"%s\",\"audio\":%s},"
        "\"blank\":%s,\"cloudMic\":%s,\"ownerMicLocal\":%s,\"tlsInsecure\":%s,"
        "\"threed\":{\"mode\":%d,\"deepness\":%d,\"convergence\":%d,\"swap\":%s,\"full\":%s,\"tier\":%d,\"ai\":\"%s\"},"
        "\"quality\":{\"w\":%d,\"h\":%d,\"bitrate\":%d},"
        "\"voice\":{\"stt\":\"%s\",\"sttModel\":\"%s\",\"sttToken\":%s,"
        "\"llm\":\"%s\",\"llmModel\":\"%s\",\"llmToken\":%s},"
        "\"sniff\":{\"want\":%s,\"mitm\":%s,\"active\":%s,\"msg\":\"%s\","
        "\"gender\":%d,\"robot\":%d,\"echo\":%d,\"whisper\":%d,\"substitute\":%s,\"relayPort\":%d},"
        "\"compctl\":{\"want\":%s,\"vision\":%s,\"active\":%s,\"msg\":\"%s\"},"
        "\"android\":%s,\"selected\":\"%s\",\"quests\":[",
        a->cloud_logged_in ? "true" : "false", esc_email, esc_name, esc_msg,
        a->internet_sharing ? "true" : "false",
        a->quest_paired ? "true" : "false", a->quest_name, a->quest_ip,
        a->streaming ? "true" : "false", a->paused ? "true" : "false",
        a->source, a->source_path, a->source_path2, a->audio ? "true" : "false",
        a->blank_want ? "true" : "false", a->cloud_mic_fallback ? "true" : "false",
        a->owner_mic_local ? "true" : "false", bsdr_tls_is_insecure() ? "true" : "false",
        a->threed_mode, a->threed_deepness, a->threed_convergence,
        a->threed_swap ? "true" : "false", a->threed_full ? "true" : "false", a->threed_tier, esc_ai,
        a->res_w, a->res_h, a->bitrate,
        a->stt_endpoint, a->stt_model, a->stt_token[0] ? "true" : "false",
        a->llm_endpoint, a->llm_model, a->llm_token[0] ? "true" : "false",
        a->sniff_want ? "true" : "false", a->sniff_mitm ? "true" : "false",
        a->sniff_active ? "true" : "false", esc_sniff,
        a->voice_gender, a->voice_robot, a->voice_echo, a->voice_whisper,
        a->voice_substitute ? "true" : "false", a->sniff_remote_port,
        a->compctl_want ? "true" : "false", a->compctl_vision ? "true" : "false",
        a->compctl_active ? "true" : "false", esc_cc,
#if defined(BSDR_PLATFORM_ANDROID)
        "true",
#else
        "false",
#endif
        a->selected_quest_ip);
    uint64_t now = bsdr_now_ms();
    for (int i = 0; i < a->quest_count && n < (int)cap - 128; i++) {
        n += snprintf(out + n, cap - n, "%s{\"ip\":\"%s\",\"name\":\"%s\",\"ageMs\":%llu}",
                      i ? "," : "", a->quests[i].ip, a->quests[i].name,
                      (unsigned long long)(now - a->quests[i].last_seen_ms));
    }
    n += snprintf(out + n, cap - n, "],");
    /* depth-model manager: cache dir, per-tier cached state + size, and any in-flight download */
    {
        char mdir[768]; bsdr_model_dir(mdir, sizeof mdir);
        char emdir[800]; bsdr_json_escape(emdir, sizeof emdir, mdir);
        bsdr_model_dl dl; bsdr_model_download_state(&dl);
        char edlerr[160]; bsdr_json_escape(edlerr, sizeof edlerr, dl.err);
        n += snprintf(out + n, cap - n, "\"models\":{\"dir\":\"%s\",\"tiers\":[", emdir);
        for (int t = 1; t <= 3; t++) {
            const bsdr_model_info *mi = bsdr_model_for_tier(t);
            n += snprintf(out + n, cap - n, "%s{\"tier\":%d,\"name\":\"%s\",\"mb\":%d,\"present\":%s}",
                          t > 1 ? "," : "", t, mi->name, mi->approx_mb,
                          bsdr_model_present(t) ? "true" : "false");
        }
        n += snprintf(out + n, cap - n,
                      "],\"dl\":{\"active\":%s,\"tier\":%d,\"pct\":%d,\"done\":%ld,\"total\":%ld,\"ok\":%s,\"err\":\"%s\"}}",
                      dl.active ? "true" : "false", dl.tier, dl.pct, dl.done, dl.total,
                      dl.ok ? "true" : "false", edlerr);
    }
    {
        char esrc[600], efst[200];
        bsdr_json_escape(esrc, sizeof esrc, a->faceswap_source);
        bsdr_json_escape(efst, sizeof efst, a->faceswap_status);
        n += snprintf(out + n, cap - n,
                      ",\"faceswap\":{\"on\":%s,\"tier\":%d,\"source\":\"%s\",\"status\":\"%s\"}",
                      a->faceswap_on ? "true" : "false", a->faceswap_tier, esrc, efst);
    }
    n += snprintf(out + n, cap - n, "}");
    bsdr_mutex_unlock(a->lock);
    return (size_t)n;
}
