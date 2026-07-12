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
#include "bsdr/inject.h"
#include "bsdr/cloud.h"
#include "bsdr/cloud_stream.h"
#include "bsdr/botroom.h"
#include "bsdr/botaudio.h"
#include "bsdr/voicestore.h"
#include "bsdr/voiceai.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/model_store.h"
#include "bsdr/micsniff.h"
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
    a->sniff_wifi = -1;     /* unknown until first status build (then cached) */
#if defined(__ANDROID__)
    a->sniff_method = 2;    /* Android has no local packet capture — only the router relay works */
#endif
    a->threed_deepness = 35; a->threed_convergence = 0;   /* comfortable defaults when 3D is enabled */
    a->threed_full = 0;      /* default: light half-SBS (full-res is ~4x the load; opt-in) */
    a->voice_fx_on = true;   /* voice changer master enable (sliders still default to 0 = no effect) */
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "not logged in");
    snprintf(a->bot_mode, sizeof(a->bot_mode), "audio");   /* default: REST join only (owner-mic unlock) */
    a->bot_solo_owner = true;     /* cloud-mic loopback defaults to "listen only to me" */
    a->voiceai_tier = 1;          /* AI voice tier default: CPU */
    a->cloud_auto_share = true;   /* follow the Quest's RDC screen (auto start/stop sharing) */
    /* Cloud VIDEO is ON: the relay video is plain H.264 (NOT encrypted, as first thought) but uses
     * Bigscreen's CUSTOM raw fragmentation (not FU-A) — reversed from full.pcapng. bsdr_video_send_au_cloud
     * matches it. Cloud AUDIO is plain Opus RTP (pt 100, djb2 SSRC, ts+=480) + the 8-byte BigSoup
     * trailer ([u32 ssrc LE][u32 frame_id LE], no XOR) so the Quest reads our SSRC instead of 0.
     * Confirmed working on a live Quest; ON by default like video — disable with --no-cloud-audio. */
    a->cloud_no_video = false;
    a->video_decoupled = false;    /* default: couple cloud to the single LAN encoder */
#if defined(__linux__) && !defined(BSDR_PLATFORM_ANDROID)
    a->cpu_only = true;            /* Linux (desktop) default: CPU-scale + libx264 encode — sharper
                                    * low-bitrate text than NVENC (which lacks psy-RD). --gpu / the web
                                    * toggle opts into the CUDA/NVENC pipeline for offload / high bitrate. */
#else
    a->cpu_only = false;           /* Windows/macOS/Android: hardware encoder by default */
#endif
    a->use_vaapi = false;          /* default off: opt-in VAAPI iGPU encode */
    a->use_kmsgrab = false;        /* default off: opt-in DRM/KMS capture */
    a->cloud_no_audio = false;
    a->audio = true;
    a->bitrate = 8000000;     /* 8 Mbps default (effective) */
    a->quest_bitrate = 8000000;
    a->bitrate_override = 0;   /* 0 = follow the headset; web UI can force a value */
    a->res_w = 0;             /* width auto-derived from the desktop aspect ratio */
    a->res_h = 720;           /* 720p default; the headset overrides via PUT /device */
}

/* Effective encode bitrate = the web override if set, else what the headset asked for, then bounded
 * by --max-bitrate. Callers hold a->lock. Everything downstream (streamer, cloud, status) reads the
 * single resolved a->bitrate, so the override is honored everywhere without threading it through. */
static void recompute_bitrate_locked(bsdr_app *a) {
    int eff = a->bitrate_override > 0 ? a->bitrate_override : a->quest_bitrate;
    if (eff <= 0) eff = 8000000;
    if (a->max_bitrate > 0 && eff > a->max_bitrate) eff = a->max_bitrate;
    a->bitrate = eff;
}

void bsdr_app_set_quality(bsdr_app *a, int w, int h, int bitrate) {
    bsdr_mutex_lock(a->lock);
    if (w >= 0) a->res_w = w;
    if (h >= 0) a->res_h = h;
    /* The headset's bitrate is a fixed cycle (1/3/5/8/.../100 Mbps) and it forces 3 Mbps when
     * internet sharing turns on. We remember it raw as quest_bitrate; the effective encode bitrate
     * (recomputed below) is the web override if one is set, else this, then capped by --max-bitrate. */
    if (bitrate > 0) a->quest_bitrate = bitrate;
    recompute_bitrate_locked(a);
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

static void settings_save(bsdr_app *a);   /* defined with the config-file helpers below */

void bsdr_app_set_bitrate_override(bsdr_app *a, int bitrate) {
    bsdr_mutex_lock(a->lock);
    a->bitrate_override = bitrate > 0 ? bitrate : 0;
    recompute_bitrate_locked(a);
    int eff = a->bitrate;
    bsdr_mutex_unlock(a->lock);
    if (a->bitrate_override > 0)
        BSDR_INFO("bsdr.app", "bitrate override: %d bps (headset config ignored)", eff);
    else
        BSDR_INFO("bsdr.app", "bitrate override cleared -> following the headset (%d bps)", eff);
    settings_save(a);   /* remember across restarts */
}

int bsdr_app_get_bitrate_override(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    int v = a->bitrate_override;
    bsdr_mutex_unlock(a->lock);
    return v;
}

/* Encoder choice: gpu=true -> CUDA/NVENC pipeline; gpu=false -> CPU-scale + libx264 (sharper text).
 * Bumps encoder_gen so a running live session reopens the capture IN PLACE — NOT source_gen, which
 * tore the whole session down (dropping the Quest input channel and stalling video for seconds). */
void bsdr_app_set_gpu_encode(bsdr_app *a, bool gpu) {
    bsdr_mutex_lock(a->lock);
    a->cpu_only = !gpu;
    a->encoder_gen++;   /* live session reopens the capture (in place) with the new encoder */
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "encoder set to %s", gpu ? "GPU (NVENC)" : "CPU (libx264)");
    settings_save(a);
}
/* VAAPI (iGPU encode) + kmsgrab (DRM/KMS capture) — Linux only. Both bump encoder_gen so a running
 * session reopens the capture in place (the encoder_gen reconfig re-reads use_vaapi/use_kmsgrab). */
void bsdr_app_set_vaapi(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock); a->use_vaapi = on; a->encoder_gen++; bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "VAAPI iGPU encode %s", on ? "on" : "off");
    settings_save(a);
}
void bsdr_app_set_kmsgrab(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock); a->use_kmsgrab = on; a->encoder_gen++; bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "kmsgrab DRM/KMS capture %s", on ? "on" : "off");
    settings_save(a);
}

bool bsdr_app_get_gpu_encode(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bool gpu = !a->cpu_only;
    bsdr_mutex_unlock(a->lock);
    return gpu;
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
    settings_save(a);   /* persist STT/LLM endpoint+model+token across restarts */
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

static void settings_save(bsdr_app *a);   /* defined below */

void bsdr_app_set_blank(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->blank_want = on;
    bsdr_mutex_unlock(a->lock);
}

/* Input pointer mode: 0 = mouse (tap = click, hold+move = drag), 1 = real touch events. Process-global
 * in the injector (per-session), so apply immediately and persist the preference. */
void bsdr_app_set_pointer_touch(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->pointer_touch = on;
    bsdr_mutex_unlock(a->lock);
    bsdr_injector_touch_mode(on ? 1 : 0);
    settings_save(a);
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

/* Copy the current Bigscreen access token (for a room-join REST call). Empty if not logged in. */
void bsdr_app_get_access_token(bsdr_app *a, char *out, size_t cap) {
    bsdr_mutex_lock(a->lock);
    snprintf(out, cap, "%s", a->access_token);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_room_mic(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->room_mic_want = on;   /* the cloud room-mic thread (cloud_stream.c) polls this to (un)expose the device */
    bsdr_mutex_unlock(a->lock);
}
bool bsdr_app_get_room_mic(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bool v = a->room_mic_want;
    bsdr_mutex_unlock(a->lock);
    return v;
}

void bsdr_app_set_relay_port(bsdr_app *a, int port) {
    bsdr_mutex_lock(a->lock);
    a->sniff_remote_port = (port > 0 && port < 65536) ? port : 0;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);   /* remember the relay port across restarts */
}
int bsdr_app_get_relay_port(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    int p = a->sniff_remote_port;
    bsdr_mutex_unlock(a->lock);
    return p;
}

static int clamp100(int v, int lo) { if (v < lo) return lo; if (v > 100) return 100; return v; }

void bsdr_app_set_voicefx(bsdr_app *a, bool on, int gender, int formant, int volume,
                          int robot, int echo, int whisper, bool substitute) {
    bsdr_mutex_lock(a->lock);
    a->voice_fx_on = on;
    a->voice_gender = clamp100(gender, -100);
    a->voice_formant = clamp100(formant, -100);
    a->voice_volume = clamp100(volume, -100);
    a->voice_robot = clamp100(robot, 0);
    a->voice_echo = clamp100(echo, 0);
    a->voice_whisper = clamp100(whisper, 0);
    a->voice_substitute = substitute;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);   /* persist the voice-changer knobs across restarts */
}

/* Effective effect values: the DSP master toggle gates the DSP effects at once so the slider
 * positions survive a disable/enable (when off, they report 0/unity = no change). Substitution is
 * the shared OUTPUT routing for the whole voice pipeline, so it stays effective when EITHER engine
 * is active — the DSP changer (voice_fx_on) OR the AI voice tier (voiceai_on). Otherwise enabling
 * only the AI voice would convert to the local virtual mic but never replace the voice in the room. */
void bsdr_app_get_voicefx(bsdr_app *a, int *gender, int *formant, int *volume,
                          int *robot, int *echo, int *whisper, bool *substitute) {
    bsdr_mutex_lock(a->lock);
    bool on = a->voice_fx_on;
    if (gender) *gender = on ? a->voice_gender : 0;
    if (formant) *formant = on ? a->voice_formant : 0;
    if (volume) *volume = on ? a->voice_volume : 0;
    if (robot) *robot = on ? a->voice_robot : 0;
    if (echo) *echo = on ? a->voice_echo : 0;
    if (whisper) *whisper = on ? a->voice_whisper : 0;
    if (substitute) *substitute = (on || a->voiceai_on) && a->voice_substitute;
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_set_voiceai(bsdr_app *a, bool on, int tier, const char *voice, int key) {
    bsdr_mutex_lock(a->lock);
    a->voiceai_on = on;
    a->voiceai_tier = (tier < 1 ? 1 : (tier > 3 ? 3 : tier));
    if (voice) snprintf(a->voiceai_voice, sizeof a->voiceai_voice, "%s", voice);
    a->voiceai_key = key < -24 ? -24 : (key > 24 ? 24 : key);
    a->voiceai_gen++;                    /* trigger the reconcile to (re)load the engine */
    bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "voice AI -> %s tier=%d voice=%s key=%d", on ? "on" : "off",
              a->voiceai_tier, a->voiceai_voice[0] ? a->voiceai_voice : "(none)", a->voiceai_key);
}
void bsdr_app_get_voiceai(bsdr_app *a, bool *on, int *tier, char *voice, size_t vl, int *key) {
    bsdr_mutex_lock(a->lock);
    if (on) *on = a->voiceai_on;
    if (tier) *tier = a->voiceai_tier ? a->voiceai_tier : 1;
    if (voice && vl) snprintf(voice, vl, "%s", a->voiceai_voice);
    if (key) *key = a->voiceai_key;
    bsdr_mutex_unlock(a->lock);
}
void bsdr_app_set_voiceai_status(bsdr_app *a, const char *status) {
    if (!a || !status) return;
    bsdr_mutex_lock(a->lock);
    snprintf(a->voiceai_status, sizeof a->voiceai_status, "%s", status);
    bsdr_mutex_unlock(a->lock);
}

void bsdr_app_voice_preset_save(bsdr_app *a, int slot, const char *name) {
    if (!a || slot < 0 || slot >= 5) return;
    bsdr_mutex_lock(a->lock);
    struct bsdr_voice_preset *p = &a->voice_presets[slot];
    snprintf(p->name, sizeof p->name, "%s", (name && name[0]) ? name : "preset");
    p->ai_on = a->voiceai_on; p->tier = a->voiceai_tier; p->key = a->voiceai_key;
    snprintf(p->voice, sizeof p->voice, "%s", a->voiceai_voice);
    p->gender = a->voice_gender; p->formant = a->voice_formant; p->volume = a->voice_volume;
    p->robot = a->voice_robot; p->echo = a->voice_echo; p->whisper = a->voice_whisper;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "voice preset %d saved as '%s'", slot, name ? name : "");
}
void bsdr_app_voice_preset_apply(bsdr_app *a, int slot) {
    if (!a || slot < 0 || slot >= 5) return;
    bsdr_mutex_lock(a->lock);
    struct bsdr_voice_preset *p = &a->voice_presets[slot];
    if (!p->name[0]) { bsdr_mutex_unlock(a->lock); return; }   /* empty slot */
    a->voiceai_on = p->ai_on; a->voiceai_tier = p->tier ? p->tier : 1; a->voiceai_key = p->key;
    snprintf(a->voiceai_voice, sizeof a->voiceai_voice, "%s", p->voice);
    a->voice_gender = p->gender; a->voice_formant = p->formant; a->voice_volume = p->volume;
    a->voice_robot = p->robot; a->voice_echo = p->echo; a->voice_whisper = p->whisper;
    if (!a->voiceai_on) a->voice_fx_on = true;   /* applying a DSP preset implies the changer is on */
    a->voiceai_gen++;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "voice preset %d applied", slot);
}
void bsdr_app_voice_preset_delete(bsdr_app *a, int slot) {
    if (!a || slot < 0 || slot >= 5) return;
    bsdr_mutex_lock(a->lock);
    memset(&a->voice_presets[slot], 0, sizeof a->voice_presets[slot]);
    bsdr_mutex_unlock(a->lock);
    settings_save(a);
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
static bool config_dir(char *dir, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
#if defined(_WIN32)
    if (!home || !home[0]) home = getenv("APPDATA");   /* Windows: no HOME by default */
#endif
    if (xdg && xdg[0]) snprintf(dir, cap, "%s/bsdr_agent", xdg);
    else if (home && home[0]) snprintf(dir, cap, "%s/.config/bsdr_agent", home);
    else return false;
    bsdr_mkdir(dir);   /* best-effort; ignore EEXIST */
    return true;
}
/* Public wrapper so other modules (e.g. cloud_stream sticky-port persistence) can reach the same
 * per-user config directory without duplicating the XDG/HOME/APPDATA logic. */
bool bsdr_config_dir(char *dir, size_t cap) { return config_dir(dir, cap); }
static bool session_path(char *out, size_t cap) {
    char dir[512];
    if (!config_dir(dir, sizeof(dir))) return false;
    snprintf(out, cap, "%s/session", dir);
    return true;
}
/* User preferences that persist across restarts (encoder choice, bitrate override). Distinct from
 * the cloud `session` file. Simple key=value lines so it's easy to extend. */
static bool settings_path(char *out, size_t cap) {
    char dir[512];
    if (!config_dir(dir, sizeof(dir))) return false;
    snprintf(out, cap, "%s/settings", dir);
    return true;
}
static void settings_save(bsdr_app *a) {
    char path[600];
    if (!settings_path(path, sizeof(path))) return;
    bsdr_mutex_lock(a->lock);
    int cpu = a->cpu_only ? 1 : 0, bro = a->bitrate_override, ptouch = a->pointer_touch ? 1 : 0;
    int encp = a->enc_level, lan1x = a->lan_1x ? 1 : 0, fcap = a->fps_cap, wopt = a->wifi_opt ? 1 : 0;
    int x264t = a->enc_x264_threads, vaapi = a->use_vaapi ? 1 : 0, kms = a->use_kmsgrab ? 1 : 0;
    int smeth = a->sniff_method, sport = a->sniff_remote_port, swant = a->sniff_want ? 1 : 0;
    char bmode[8]; snprintf(bmode, sizeof bmode, "%s", a->bot_mode[0] ? a->bot_mode : "audio");
    int bfollow = a->bot_follow ? 1 : 0, bloop = a->bot_loopback ? 1 : 0, bsolo = a->bot_solo_owner ? 1 : 0;
    int vfon = a->voice_fx_on ? 1 : 0, vsub = a->voice_substitute ? 1 : 0;
    int vg = a->voice_gender, vfm = a->voice_formant, vvol = a->voice_volume;
    int vrob = a->voice_robot, vech = a->voice_echo, vwh = a->voice_whisper;
    int vaion = a->voiceai_on ? 1 : 0, vaitier = a->voiceai_tier, vaikey = a->voiceai_key;
    char vaivoice[64]; snprintf(vaivoice, sizeof vaivoice, "%s", a->voiceai_voice);
    struct bsdr_voice_preset vps[5]; memcpy(vps, a->voice_presets, sizeof vps);
    char se[512], sm[512], stk[512], le[512], lm[512], ltk[512];
    snprintf(se, sizeof se, "%s", a->stt_endpoint); snprintf(sm, sizeof sm, "%s", a->stt_model);
    snprintf(stk, sizeof stk, "%s", a->stt_token);  snprintf(le, sizeof le, "%s", a->llm_endpoint);
    snprintf(lm, sizeof lm, "%s", a->llm_model);    snprintf(ltk, sizeof ltk, "%s", a->llm_token);
    bsdr_mutex_unlock(a->lock);
    FILE *f = fopen(path, "w");
    if (!f) { BSDR_WARN("bsdr.app", "could not save settings to %s", path); return; }
    fprintf(f, "cpu_only=%d\nbitrate_override=%d\npointer_touch=%d\n"
               "sniff_method=%d\nsniff_relay_port=%d\nsniff_want=%d\nbot_mode=%s\nbot_follow=%d\n"
               "bot_loopback=%d\nbot_solo_owner=%d\nenc_level=%d\nlan_1x=%d\nfps_cap=%d\nwifi_opt=%d\n"
               "x264_threads=%d\nuse_vaapi=%d\nuse_kmsgrab=%d\n",
            cpu, bro, ptouch, smeth, sport, swant, bmode, bfollow, bloop, bsolo, encp, lan1x, fcap, wopt,
            x264t, vaapi, kms);
    /* voice changer: persist the master + every knob so presets/settings survive a restart */
    fprintf(f, "voice_fx=%d\nvoice_gender=%d\nvoice_formant=%d\nvoice_volume=%d\n"
               "voice_robot=%d\nvoice_echo=%d\nvoice_whisper=%d\nvoice_substitute=%d\n",
            vfon, vg, vfm, vvol, vrob, vech, vwh, vsub);
    /* AI voice tier + up to 5 named presets (tab-free, '|'-joined so the name can contain spaces). */
    fprintf(f, "voiceai_on=%d\nvoiceai_tier=%d\nvoiceai_key=%d\nvoiceai_voice=%s\n",
            vaion, vaitier, vaikey, vaivoice);
    for (int i = 0; i < 5; i++) {
        if (!vps[i].name[0]) continue;
        fprintf(f, "voice_preset%d=%s|%d|%d|%d|%s|%d|%d|%d|%d|%d|%d\n", i,
                vps[i].name, vps[i].ai_on, vps[i].tier, vps[i].key, vps[i].voice[0] ? vps[i].voice : "-",
                vps[i].gender, vps[i].formant, vps[i].volume, vps[i].robot, vps[i].echo, vps[i].whisper);
    }
    /* Voice/LLM config — only write non-empty keys so a blank line never clobbers a default. */
    if (se[0])  fprintf(f, "stt_endpoint=%s\n", se);
    if (sm[0])  fprintf(f, "stt_model=%s\n", sm);
    if (stk[0]) fprintf(f, "stt_token=%s\n", stk);
    if (le[0])  fprintf(f, "llm_endpoint=%s\n", le);
    if (lm[0])  fprintf(f, "llm_model=%s\n", lm);
    if (ltk[0]) fprintf(f, "llm_token=%s\n", ltk);
    fclose(f);
    BSDR_DEBUG("bsdr.app", "settings saved to %s", path);
}
/* Load persisted prefs at startup — call AFTER bsdr_app_init (platform defaults) and BEFORE applying
 * CLI flags, so an explicit --cpu/--gpu on the command line still wins over the saved value. */
void bsdr_app_load_settings(bsdr_app *a) {
    char path[600];
    if (!settings_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600]; int v;
    bsdr_mutex_lock(a->lock);
    while (fgets(line, sizeof(line), f)) {
        size_t ll = strlen(line);                       /* strip trailing newline for the string fields */
        while (ll && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = '\0';
        /* widths below are (field size - 1) so sscanf can never overflow the destination */
        if      (sscanf(line, "cpu_only=%d", &v) == 1)         a->cpu_only = v ? true : false;
        else if (sscanf(line, "bitrate_override=%d", &v) == 1) a->bitrate_override = v > 0 ? v : 0;
        else if (sscanf(line, "pointer_touch=%d", &v) == 1)    a->pointer_touch = v ? true : false;
        else if (sscanf(line, "sniff_method=%d", &v) == 1)     a->sniff_method = (v >= 0 && v <= 2) ? v : 0;
        else if (sscanf(line, "sniff_relay_port=%d", &v) == 1) a->sniff_remote_port = (v > 0 && v < 65536) ? v : 0;
        else if (sscanf(line, "sniff_want=%d", &v) == 1)       a->sniff_want = v ? true : false;
        else if (sscanf(line, "enc_level=%d", &v) == 1)        a->enc_level = v < 0 ? 0 : v > 2 ? 2 : v;
        else if (sscanf(line, "enc_perf=%d", &v) == 1)         a->enc_level = v ? 2 : 0;  /* legacy bool -> level */
        else if (sscanf(line, "x264_threads=%d", &v) == 1)     a->enc_x264_threads = v < 0 ? 0 : v > 32 ? 32 : v;
        else if (sscanf(line, "use_vaapi=%d", &v) == 1)        a->use_vaapi = v ? true : false;
        else if (sscanf(line, "use_kmsgrab=%d", &v) == 1)      a->use_kmsgrab = v ? true : false;
        else if (sscanf(line, "lan_1x=%d", &v) == 1)           a->lan_1x = v ? true : false;
        else if (sscanf(line, "fps_cap=%d", &v) == 1)          a->fps_cap = (v >= 0 && v <= 120) ? v : 0;
        else if (sscanf(line, "wifi_opt=%d", &v) == 1)         a->wifi_opt = v ? true : false;
        else if (sscanf(line, "voice_fx=%d", &v) == 1)         a->voice_fx_on = v ? true : false;
        else if (sscanf(line, "voice_gender=%d", &v) == 1)     a->voice_gender = (v < -100 ? -100 : v > 100 ? 100 : v);
        else if (sscanf(line, "voice_formant=%d", &v) == 1)    a->voice_formant = (v < -100 ? -100 : v > 100 ? 100 : v);
        else if (sscanf(line, "voice_volume=%d", &v) == 1)     a->voice_volume = (v < -100 ? -100 : v > 100 ? 100 : v);
        else if (sscanf(line, "voice_robot=%d", &v) == 1)      a->voice_robot = (v < 0 ? 0 : v > 100 ? 100 : v);
        else if (sscanf(line, "voice_echo=%d", &v) == 1)       a->voice_echo = (v < 0 ? 0 : v > 100 ? 100 : v);
        else if (sscanf(line, "voice_whisper=%d", &v) == 1)    a->voice_whisper = (v < 0 ? 0 : v > 100 ? 100 : v);
        else if (sscanf(line, "voice_substitute=%d", &v) == 1) a->voice_substitute = v ? true : false;
        else if (sscanf(line, "voiceai_on=%d", &v) == 1)       a->voiceai_on = v ? true : false;
        else if (sscanf(line, "voiceai_tier=%d", &v) == 1)     a->voiceai_tier = (v < 1 ? 1 : v > 3 ? 3 : v);
        else if (sscanf(line, "voiceai_key=%d", &v) == 1)      a->voiceai_key = (v < -24 ? -24 : v > 24 ? 24 : v);
        else if (sscanf(line, "voiceai_voice=%63[^\n]", a->voiceai_voice) == 1) continue;
        else if (strncmp(line, "voice_preset", 12) == 0) {
            int slot = -1; char rest[400] = "";
            if (sscanf(line, "voice_preset%d=%399[^\n]", &slot, rest) == 2 && slot >= 0 && slot < 5) {
                struct bsdr_voice_preset *p = &a->voice_presets[slot];
                char nm[40] = "", voi[64] = "";
                int aion = 0, tier = 1, key = 0, g = 0, fm = 0, vol = 0, rb = 0, ec = 0, wh = 0;
                if (sscanf(rest, "%39[^|]|%d|%d|%d|%63[^|]|%d|%d|%d|%d|%d|%d",
                           nm, &aion, &tier, &key, voi, &g, &fm, &vol, &rb, &ec, &wh) >= 5) {
                    snprintf(p->name, sizeof p->name, "%s", nm);
                    p->ai_on = aion; p->tier = tier; p->key = key;
                    snprintf(p->voice, sizeof p->voice, "%s", strcmp(voi, "-") ? voi : "");
                    p->gender = g; p->formant = fm; p->volume = vol; p->robot = rb; p->echo = ec; p->whisper = wh;
                }
            }
        }
        else if (sscanf(line, "bot_mode=%7[^\n]", a->bot_mode) == 1) {
            if (strcmp(a->bot_mode, "full") != 0 && strcmp(a->bot_mode, "audio") != 0)
                snprintf(a->bot_mode, sizeof a->bot_mode, "audio");
        }
        else if (sscanf(line, "bot_follow=%d", &v) == 1) a->bot_follow = v ? true : false;
        else if (sscanf(line, "bot_loopback=%d", &v) == 1) a->bot_loopback = v ? true : false;
        else if (sscanf(line, "bot_solo_owner=%d", &v) == 1) a->bot_solo_owner = v ? true : false;
        else if (sscanf(line, "stt_endpoint=%255[^\n]", a->stt_endpoint) == 1) continue;
        else if (sscanf(line, "stt_model=%63[^\n]", a->stt_model) == 1) continue;
        else if (sscanf(line, "stt_token=%255[^\n]", a->stt_token) == 1) continue;
        else if (sscanf(line, "llm_endpoint=%255[^\n]", a->llm_endpoint) == 1) continue;
        else if (sscanf(line, "llm_model=%63[^\n]", a->llm_model) == 1) continue;
        else if (sscanf(line, "llm_token=%255[^\n]", a->llm_token) == 1) continue;
    }
#if defined(__ANDROID__)
    a->sniff_method = 2;                        /* Android: relay only, whatever was saved */
#endif
    a->sniff_mitm = (a->sniff_method == 1);    /* derived flag the reconcile reads */
    /* If the owner mic was left ON, ask the main loop to bring it back up on this boot. Relay needs no
     * privilege; passive/MITM will (re)prompt for the sudo password via the web UI if not already root. */
    if (a->sniff_want) a->sniff_dirty = true;
    recompute_bitrate_locked(a);
    bool ptouch = a->pointer_touch;
    bsdr_mutex_unlock(a->lock);
    bsdr_injector_touch_mode(ptouch ? 1 : 0);   /* apply the saved pointer mode */
    fclose(f);
    BSDR_INFO("bsdr.app", "loaded settings from %s (encoder=%s, bitrate override=%d)",
              path, a->cpu_only ? "cpu/x264" : "gpu/nvenc", a->bitrate_override);
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
    bool ok = bsdr_cloud_login(0, email, password, &res);   /* blocking HTTPS */
    char name[128] = "";
    if (ok) bsdr_cloud_account(bsdr_cloud_api_key(), res.access_token, name, sizeof(name));

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
        bsdr_cloud_ws *ws = bsdr_cloud_ws_open(res.access_token, 0);
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
    bool valid = bsdr_cloud_account(bsdr_cloud_api_key(), access, vname, sizeof(vname));
    if (!valid && refresh[0]) {
        BSDR_INFO("bsdr.app", "saved token expired — renewing");
        bsdr_cloud_result rr;
        if (bsdr_cloud_renew(bsdr_cloud_api_key(), refresh, &rr)) {
            snprintf(access, sizeof(access), "%s", rr.access_token);
            if (rr.refresh_token[0]) snprintf(refresh, sizeof(refresh), "%s", rr.refresh_token);
            valid = bsdr_cloud_account(bsdr_cloud_api_key(), access, vname, sizeof(vname));
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
    bsdr_cloud_ws *ws = bsdr_cloud_ws_open(a->access_token, 0);
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

/* ---- second "bot" account: login / restore / logout / join-room (mirrors the host, no media) ---- */
static bool bot_session_path(char *out, size_t cap) {
    char dir[512];
    if (!config_dir(dir, sizeof(dir))) return false;
    snprintf(out, cap, "%s/bot_session", dir);
    return true;
}
static void bot_session_save(bsdr_app *a) {
    char path[600];
    if (!bot_session_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "w");
    if (!f) { BSDR_WARN("bsdr.app", "could not save bot session"); return; }
#if !defined(_WIN32)
    fchmod(fileno(f), 0600);
#endif
    fprintf(f, "%s\n%s\n%s\n%s\n", a->bot_access_token, a->bot_refresh_token, a->bot_email, a->bot_name);
    fclose(f);
}
static void bot_session_clear(void) { char p[600]; if (bot_session_path(p, sizeof(p))) remove(p); }

void bsdr_app_bot_login(bsdr_app *a, const char *email, const char *password) {
    bsdr_mutex_lock(a->lock);
    snprintf(a->bot_email, sizeof(a->bot_email), "%s", email);
    snprintf(a->bot_msg, sizeof(a->bot_msg), "logging in...");
    bsdr_mutex_unlock(a->lock);

    bsdr_cloud_result res;
    bool ok = bsdr_cloud_login(1, email, password, &res);   /* blocking HTTPS */
    char name[128] = "";
    if (ok) bsdr_cloud_account(bsdr_cloud_client_key(), res.access_token, name, sizeof(name));

    bsdr_mutex_lock(a->lock);
    a->bot_logged_in = ok;
    if (ok) a->bot_stopped = false;
    snprintf(a->bot_msg, sizeof(a->bot_msg), "%.150s", res.message);
    if (ok) {
        snprintf(a->bot_access_token, sizeof(a->bot_access_token), "%s", res.access_token);
        snprintf(a->bot_refresh_token, sizeof(a->bot_refresh_token), "%s", res.refresh_token);
        snprintf(a->bot_name, sizeof(a->bot_name), "%s", name[0] ? name : email);
    }
    bsdr_mutex_unlock(a->lock);

    if (ok) {
        char sid[80] = ""; bsdr_cloud_my_socialid(res.access_token, sid, sizeof sid);
        bsdr_mutex_lock(a->lock); snprintf(a->bot_social_id, sizeof a->bot_social_id, "%s", sid); bsdr_mutex_unlock(a->lock);
        bot_session_save(a);
        /* Presence WS: the room-participants count is fanned to sessions the server considers present,
         * so keep the bot's WS open (same reason as the host). */
        bsdr_cloud_ws *ws = bsdr_cloud_ws_open(res.access_token, 1);
        bsdr_mutex_lock(a->lock); a->bot_ws = ws; bsdr_mutex_unlock(a->lock);
        BSDR_INFO("bsdr.app", "bot account logged in as %s (socialId=%s)", name[0] ? name : email, sid[0] ? sid : "?");
    }
}

void bsdr_app_bot_restore(bsdr_app *a) {
    char path[600];
    if (!bot_session_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char access[2048] = "", refresh[2048] = "", email[128] = "", name[128] = "";
    if (!fgets(access, sizeof(access), f)) { fclose(f); return; }
    if (!fgets(refresh, sizeof(refresh), f)) refresh[0] = 0;
    if (!fgets(email, sizeof(email), f))     email[0] = 0;
    if (!fgets(name, sizeof(name), f))       name[0] = 0;
    fclose(f);
    access[strcspn(access, "\r\n")] = 0; refresh[strcspn(refresh, "\r\n")] = 0;
    email[strcspn(email, "\r\n")] = 0;   name[strcspn(name, "\r\n")] = 0;
    if (!access[0]) return;

    char vname[128] = "";
    bool valid = bsdr_cloud_account(bsdr_cloud_client_key(), access, vname, sizeof(vname));
    if (!valid && refresh[0]) {
        bsdr_cloud_result rr;
        if (bsdr_cloud_renew(bsdr_cloud_client_key(), refresh, &rr)) {
            snprintf(access, sizeof(access), "%s", rr.access_token);
            if (rr.refresh_token[0]) snprintf(refresh, sizeof(refresh), "%s", rr.refresh_token);
            valid = bsdr_cloud_account(bsdr_cloud_client_key(), access, vname, sizeof(vname));
        }
    }
    if (!valid) { BSDR_INFO("bsdr.app", "saved bot session invalid; log the bot in again"); return; }

    bsdr_mutex_lock(a->lock);
    a->bot_logged_in = true;
    a->bot_stopped = false;
    snprintf(a->bot_access_token, sizeof(a->bot_access_token), "%s", access);
    snprintf(a->bot_refresh_token, sizeof(a->bot_refresh_token), "%s", refresh);
    snprintf(a->bot_email, sizeof(a->bot_email), "%s", email);
    snprintf(a->bot_name, sizeof(a->bot_name), "%s", vname[0] ? vname : (name[0] ? name : email));
    snprintf(a->bot_msg, sizeof(a->bot_msg), "logged in (restored)");
    bsdr_mutex_unlock(a->lock);
    bot_session_save(a);
    bsdr_cloud_ws *ws = bsdr_cloud_ws_open(access, 1);
    bsdr_mutex_lock(a->lock); a->bot_ws = ws; bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "restored bot account %s", vname[0] ? vname : email);
}

static void bot_set_msg(bsdr_app *a, bool joined, const char *room, const char *fmt, ...);
static void bot_loopback_apply(bsdr_app *a);

void bsdr_app_bot_logout(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bsdr_cloud_ws *ws = a->bot_ws;
    bsdr_botroom *room = (bsdr_botroom *)a->bot_room; a->bot_room = NULL;
    bsdr_botaudio *ba = (bsdr_botaudio *)a->bot_audio; a->bot_audio = NULL;
    a->bot_ws = NULL; a->bot_logged_in = false; a->bot_joined = false;
    a->bot_access_token[0] = a->bot_refresh_token[0] = a->bot_name[0] = a->bot_email[0] = a->bot_room_id[0] = 0;
    snprintf(a->bot_msg, sizeof(a->bot_msg), "not logged in");
    bsdr_mutex_unlock(a->lock);
    if (room) bsdr_botroom_stop(room);   /* tear down the avatar presence */
    if (ba) bsdr_botaudio_stop(ba);      /* tear down the cloud-mic loopback */
    bot_session_clear();
    if (ws) bsdr_cloud_ws_close(ws);
    BSDR_INFO("bsdr.app", "bot account logged out");
}

/* Leave the current room but stay logged in (WS/online presence up) — undo a Join. Tears the avatar
 * down and drops the room participation so the owner-mic gate re-locks; the bot can Join again. */
bool bsdr_app_bot_leave_room(bsdr_app *a) {
    char bot_tok[2048], room[128];
    bsdr_mutex_lock(a->lock);
    bool logged = a->bot_logged_in;
    bsdr_botroom *br = (bsdr_botroom *)a->bot_room; a->bot_room = NULL;
    bsdr_botaudio *ba = (bsdr_botaudio *)a->bot_audio; a->bot_audio = NULL;
    snprintf(bot_tok, sizeof bot_tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    bsdr_mutex_unlock(a->lock);
    if (br) bsdr_botroom_stop(br);   /* stop broadcasting the avatar first */
    if (ba) bsdr_botaudio_stop(ba);  /* stop the cloud-mic loopback */
    if (!logged) { bot_set_msg(a, false, NULL, "bot is not logged in"); return false; }
    int rc = room[0] ? bsdr_cloud_leave_room(bot_tok, room) : 0;
    bot_set_msg(a, false, NULL, (rc / 100 == 2 || !room[0]) ? "left the room" : "left the room (HTTP %d)", rc);
    BSDR_INFO("bsdr.app", "bot leave-room %s -> HTTP %d", room[0] ? room : "(none)", rc);
    return true;
}

/* Stop the bot: leave the room, tear the avatar down, and close the WS/online presence — but KEEP the
 * saved login (token + persisted session) so Start brings it back in one click (no re-typing creds). */
void bsdr_app_bot_stop(bsdr_app *a) {
    char bot_tok[2048], room[128];
    bsdr_mutex_lock(a->lock);
    bsdr_cloud_ws *ws = a->bot_ws; a->bot_ws = NULL;
    bsdr_botroom *br = (bsdr_botroom *)a->bot_room; a->bot_room = NULL;
    bsdr_botaudio *ba = (bsdr_botaudio *)a->bot_audio; a->bot_audio = NULL;
    snprintf(bot_tok, sizeof bot_tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    a->bot_logged_in = false; a->bot_joined = false; a->bot_stopped = true;
    a->bot_room_id[0] = 0;
    snprintf(a->bot_msg, sizeof a->bot_msg, "stopped — login remembered (Start to reconnect)");
    bsdr_mutex_unlock(a->lock);
    if (br) bsdr_botroom_stop(br);
    if (ba) bsdr_botaudio_stop(ba);                      /* stop the cloud-mic loopback */
    if (room[0]) bsdr_cloud_leave_room(bot_tok, room);   /* best-effort: drop the room participation */
    if (ws) bsdr_cloud_ws_close(ws);                     /* drop online presence (keeps the saved session) */
    BSDR_INFO("bsdr.app", "bot stopped (login remembered)");
}

/* Start (reconnect) a stopped bot from the remembered session — no password. */
void bsdr_app_bot_start(bsdr_app *a) {
    bsdr_mutex_lock(a->lock); a->bot_stopped = false; bsdr_mutex_unlock(a->lock);
    bsdr_app_bot_restore(a);   /* re-validates the saved token + re-opens the presence WS */
}

/* Get the bot into the HOST's current room so Room.participants > 1 unlocks the owner mic, honoring the
 * user's privacy policy ([[bsdrx-bot-join-room-policy]]): open -> join directly; friends/verified/invite
 * -> host invites the bot -> bot accepts (stages its socialId) -> bot joins, NO privacy change; only a
 * fully-closed (AdminsOnly) room is minimally raised to invite-only first. */
static void bot_set_msg(bsdr_app *a, bool joined, const char *room, const char *fmt, ...) {
    char m[160]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    bsdr_mutex_lock(a->lock);
    a->bot_joined = joined;
    if (room) snprintf(a->bot_room_id, sizeof a->bot_room_id, "%s", room);
    snprintf(a->bot_msg, sizeof a->bot_msg, "%s", m);
    bsdr_mutex_unlock(a->lock);
}
bool bsdr_app_bot_join_room(bsdr_app *a) {
    char host_tok[2048], bot_tok[2048], bot_sid[80], mode[8];
    bsdr_mutex_lock(a->lock);
    int have = a->cloud_logged_in && a->bot_logged_in;
    snprintf(host_tok, sizeof host_tok, "%s", a->access_token);
    snprintf(bot_tok,  sizeof bot_tok,  "%s", a->bot_access_token);
    snprintf(bot_sid,  sizeof bot_sid,  "%s", a->bot_social_id);
    snprintf(mode,     sizeof mode,     "%s", a->bot_mode[0] ? a->bot_mode : "audio");
    bsdr_mutex_unlock(a->lock);
    if (!have) { bot_set_msg(a, false, NULL, "need BOTH the host and the bot logged in"); return false; }
    if (!bot_sid[0]) { bsdr_cloud_my_socialid(bot_tok, bot_sid, sizeof bot_sid);
        bsdr_mutex_lock(a->lock); snprintf(a->bot_social_id, sizeof a->bot_social_id, "%s", bot_sid); bsdr_mutex_unlock(a->lock); }

    /* host's current room + its join policy */
    bsdr_cloud_screen room; memset(&room, 0, sizeof room);
    if (!bsdr_cloud_get_rooms(host_tok, &room) || !room.room_id[0]) {
        bot_set_msg(a, false, NULL, "host is not in a room"); return false;
    }
    const char *ut = room.user_type[0] ? room.user_type : "Anyone";
    BSDR_INFO("bsdr.app", "bot join: room %s preferredUserType=%s botSocialId=%s", room.room_id, ut, bot_sid);

    /* Policy: only a fully-closed (AdminsOnly) room is minimally opened — to VerifiedUsersOnly (the
     * lowest tier that still honors invite staging). Everything else is left as-is. */
    if (strcmp(ut, "AdminsOnly") == 0) {
        int sc = bsdr_cloud_set_room_usertype(host_tok, room.room_id, "VerifiedUsersOnly");
        if (sc / 100 != 2) { bot_set_msg(a, false, room.room_id,
            "room is closed (AdminsOnly) and raising it to invite-only failed (HTTP %d)", sc); return false; }
        snprintf(room.user_type, sizeof room.user_type, "VerifiedUsersOnly"); ut = room.user_type;
    }

    /* 1) TRY A DIRECT JOIN FIRST. This succeeds whenever the bot is already eligible — an open room, or
     * the bot being the owner / verified / a friend / already staged. No invite needed (a public room
     * must not require one). Only if the server actually refuses do we fall back to the invite flow. */
    bsdr_cloud_screen peer; memset(&peer, 0, sizeof peer);
    bool ok = bsdr_cloud_join_room(bot_tok, room.room_id, &peer);

    /* 2) Refused on a non-open room -> the bot must be STAGED (in the room's stagedSocialIds), which
     * happens when it ACCEPTS a RoomInvite. Prefer an invite already sent from the Bigscreen app (no
     * socialId needed); only if none is pending do we try to create one (which needs the bot's socialId
     * + friendship, and /auth/account doesn't expose socialId — so that path is best-effort). */
    if (!ok && strcmp(ut, "Anyone") != 0) {
        BSDR_INFO("bsdr.app", "bot direct-join refused (HTTP %d); need staging via an accepted RoomInvite", peer.http_status);
        char nid[128] = "", inv_room[128] = "";   /* matches bsdr_cloud_screen.room_id so the copy can't truncate */
        int found = bsdr_cloud_find_room_invite(bot_tok, nid, sizeof nid, inv_room, sizeof inv_room);
        if (!found) {
            /* No pending invite -> try to send one host->bot. Needs the bot socialId (best-effort). */
            if (!bot_sid[0]) { bsdr_cloud_my_socialid(bot_tok, bot_sid, sizeof bot_sid);
                bsdr_mutex_lock(a->lock); snprintf(a->bot_social_id, sizeof a->bot_social_id, "%s", bot_sid); bsdr_mutex_unlock(a->lock); }
            if (!bot_sid[0]) { bot_set_msg(a, false, room.room_id,
                "join refused (HTTP %d): the bot isn't authorized and its socialId could not be fetched (GET /social/profile failed). INVITE it to the room from the Bigscreen app, then click Join again.",
                peer.http_status); return false; }
            int ic = bsdr_cloud_create_notification(host_tok, bot_sid, "RoomInvite");
            if (ic / 100 != 2) { bot_set_msg(a, false, room.room_id,
                "invite failed (HTTP %d) — invite the bot from the app, or make the accounts friends first", ic); return false; }
            for (int t = 0; t < 8 && !found; t++) {
                if (bsdr_cloud_find_room_invite(bot_tok, nid, sizeof nid, inv_room, sizeof inv_room)) found = 1;
                else bsdr_sleep_ms(1000);
            }
            if (!found) { bot_set_msg(a, false, room.room_id, "invite sent but the bot never received it (check friendship/timing)"); return false; }
        }
        int ac = bsdr_cloud_notification_action(bot_tok, nid, "accept");
        /* HTTP 409 "Notification already accepted" means the invite was consumed in a prior run and the
         * bot is already staged — that is not a failure, it IS accepted. Treat 2xx OR 409 as success. */
        if (ac / 100 != 2 && ac != 409) { bot_set_msg(a, false, room.room_id, "bot could not accept the invite (HTTP %d)", ac); return false; }
        /* Accepting the RoomInvite IS the join: the bot is now a room participant (Room.participants
         * rises, unlocking the owner mic). So a successful accept = success — do NOT report an error.
         * The explicit /room/{id}/join below only fetches the bot's OWN mic peer and 404s once you're
         * already in; run it best-effort and ignore its result. */
        ok = true;
        if (inv_room[0]) snprintf(room.room_id, sizeof room.room_id, "%s", inv_room);
        memset(&peer, 0, sizeof peer);
        bsdr_cloud_join_room(bot_tok, room.room_id, &peer);   /* best-effort mic peer; failure is fine */
    }

    /* Full-bot mode: after a successful join, bring up the room data plane so an avatar renders.
     * The bot's own MediaPeer (relay ip + dataPort) comes back in the join response; if the server
     * didn't include one (some join responses carry only MediaServerInfo), fall back to the room's
     * presenting-screen relay from GET /rooms — same mediasoup, same dataPort semantics. */
    if (ok && strcmp(mode, "full") == 0) {
        const char *rip = peer.media_ip[0] ? peer.media_ip : room.media_ip;
        int dport = peer.data_port ? peer.data_port : room.data_port;
        /* The avatar's data-channel prefix MUST be our room legacyUserId — the Quest keys remote avatars
         * by it (RemoteUsersManager dictionary). Prefer the join response; if it didn't carry one, fetch
         * the full room state (localUser.legacyUserId). Without it, an avatar cannot render — the Quest
         * drops every UserState/TickState whose prefix it can't resolve. */
        char legacy[64] = "";
        snprintf(legacy, sizeof legacy, "%s", peer.legacy_user_id);
        if (!legacy[0] && room.room_id[0]) {
            bsdr_cloud_screen full; memset(&full, 0, sizeof full);
            if (bsdr_cloud_get_room(bot_tok, room.room_id, &full) && full.legacy_user_id[0])
                snprintf(legacy, sizeof legacy, "%s", full.legacy_user_id);
        }
        /* replace any presence from a previous join */
        bsdr_mutex_lock(a->lock);
        bsdr_botroom *old_room = (bsdr_botroom *)a->bot_room; a->bot_room = NULL;
        bsdr_mutex_unlock(a->lock);
        if (old_room) bsdr_botroom_stop(old_room);
        if (rip && rip[0] && dport > 0 && legacy[0]) {
            BSDR_INFO("bsdr.app", "full-bot: avatar prefix legacyUserId=%s relay=%s data=%d", legacy, rip, dport);
            bsdr_botroom *br = bsdr_botroom_start(rip, dport, legacy, /*seat*/-1);
            bsdr_mutex_lock(a->lock); a->bot_room = br; bsdr_mutex_unlock(a->lock);
            if (br) bot_set_msg(a, true, room.room_id, "joined the host's room (full bot: avatar up)");
            else    bot_set_msg(a, true, room.room_id, "joined (avatar transport unavailable — audio-only)");
        } else if (rip && rip[0] && dport > 0) {
            BSDR_WARN("bsdr.app", "full-bot: no legacyUserId from the cloud (join + GET /room both lacked it) "
                      "— the avatar can't render; staying joined audio-only. Capture the room JSON to confirm the key.");
            bot_set_msg(a, true, room.room_id, "joined (no legacyUserId for the avatar — check the log)");
        } else {
            BSDR_WARN("bsdr.app", "full-bot: join returned no data peer (relay=%s data=%d) — avatar skipped",
                      rip ? rip : "(none)", dport);
            bot_set_msg(a, true, room.room_id, "joined (no data peer for the avatar — check the log)");
        }
    } else if (ok) {
        bot_set_msg(a, true, room.room_id, "joined the host's room");
    } else {
        bot_set_msg(a, false, room.room_id, "room-join failed (HTTP %d)", peer.http_status);
    }
    /* Capture the bot's own room-audio peer (+ the room owner's session id for the solo) so the
     * cloud-mic loopback can consume it — starts now if the loopback toggle is on. */
    if (ok) {
        const char *aip = peer.media_ip[0] ? peer.media_ip : room.media_ip;
        int aport = peer.audio_port ? peer.audio_port : room.audio_port;
        bsdr_mutex_lock(a->lock);
        snprintf(a->bot_audio_ip, sizeof a->bot_audio_ip, "%s", aip ? aip : "");
        a->bot_audio_port = aport;
        snprintf(a->bot_owner_sid, sizeof a->bot_owner_sid, "%s", room.session_id);
        bsdr_mutex_unlock(a->lock);
        bot_loopback_apply(a);
    }
    BSDR_INFO("bsdr.app", "bot join-room %s (%s, mode=%s) -> %s", room.room_id, ut, mode, ok ? "OK" : "FAILED");
    return ok;
}

void bsdr_app_set_enc_level(bsdr_app *a, int level) {
    if (level < 0) level = 0; else if (level > 2) level = 2;
    bsdr_mutex_lock(a->lock); a->enc_level = level; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "encoder level -> %s (restart the stream to apply)",
              level >= 2 ? "performance" : level == 1 ? "balanced" : "quality");
}
void bsdr_app_set_x264_threads(bsdr_app *a, int n) {
    if (n < 0) n = 0; else if (n > 32) n = 32;
    bsdr_mutex_lock(a->lock); a->enc_x264_threads = n; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "x264 frame threads -> %d (restart the stream to apply)", n);
}

void bsdr_app_set_lan_1x(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock); a->lan_1x = on; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "LAN video redundancy -> %s", on ? "1x (halved uplink)" : "2x (default)");
}

void bsdr_app_set_fps_cap(bsdr_app *a, int fps) {
    if (fps < 0) fps = 0;
    if (fps > 120) fps = 120;
    bsdr_mutex_lock(a->lock); a->fps_cap = fps; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "fps cap -> %d (restart the stream to apply)", fps ? fps : 30);
}

void bsdr_app_set_wifi_opt(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock); a->wifi_opt = on; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "Wi-Fi network optimization -> %s (restart the stream to apply)", on ? "on (DSCP/WMM)" : "off");
}

void bsdr_app_bot_set_mode(bsdr_app *a, const char *mode) {
    if (!a || !mode) return;
    const char *m = (strcmp(mode, "full") == 0) ? "full" : "audio";
    bsdr_botroom *stop_room = NULL;
    bsdr_mutex_lock(a->lock);
    snprintf(a->bot_mode, sizeof a->bot_mode, "%s", m);
    if (strcmp(m, "audio") == 0) { stop_room = (bsdr_botroom *)a->bot_room; a->bot_room = NULL; }
    bsdr_mutex_unlock(a->lock);
    if (stop_room) bsdr_botroom_stop(stop_room);   /* downgraded to audio -> tear the avatar down */
    settings_save(a);
    BSDR_INFO("bsdr.app", "bot mode -> %s", m);
}

void bsdr_app_set_bot_follow(bsdr_app *a, bool on) {
    if (!a) return;
    bsdr_mutex_lock(a->lock); a->bot_follow = on; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "bot follow-me -> %s", on ? "on" : "off");
}

/* Start/stop the cloud-mic loopback to match desired state (bot_loopback && joined && have a peer).
 * Snapshots under the lock, then acts outside it (start/stop join threads). Safe to call repeatedly. */
static void bot_loopback_apply(bsdr_app *a) {
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    int want = a->bot_loopback && a->bot_joined && !a->bot_stopped && a->bot_audio_ip[0] && a->bot_audio_port > 0;
    bool running = a->bot_audio != NULL;
    char ip[64]; int port; char osid[200];
    snprintf(ip, sizeof ip, "%s", a->bot_audio_ip); port = a->bot_audio_port;
    snprintf(osid, sizeof osid, "%s", a->bot_owner_sid);
    bsdr_botaudio *stop_it = NULL;
    if (!want && running) { stop_it = (bsdr_botaudio *)a->bot_audio; a->bot_audio = NULL; }
    int start_it = want && !running;
    bsdr_mutex_unlock(a->lock);

    if (stop_it) bsdr_botaudio_stop(stop_it);
    if (start_it) {
        bsdr_botaudio *ba = bsdr_botaudio_start(ip, port, osid, a);
        bsdr_mutex_lock(a->lock);
        if (a->bot_audio) { bsdr_botaudio *dup = (bsdr_botaudio *)a->bot_audio; a->bot_audio = ba; bsdr_mutex_unlock(a->lock); if (dup) bsdr_botaudio_stop(dup); }
        else { a->bot_audio = ba; bsdr_mutex_unlock(a->lock); }
        BSDR_INFO("bsdr.app", "bot loopback -> %s room audio %s:%d owner=%s", ba ? "consuming" : "FAILED",
                  ip, port, osid[0] ? osid : "(none)");
    }
}

void bsdr_app_set_bot_loopback(bsdr_app *a, bool on) {
    if (!a) return;
    bsdr_mutex_lock(a->lock); a->bot_loopback = on; bsdr_mutex_unlock(a->lock);
    settings_save(a);
    BSDR_INFO("bsdr.app", "bot cloud-mic loopback -> %s", on ? "on" : "off");
    bot_loopback_apply(a);   /* start/stop immediately if the bot is already joined */
}

void bsdr_app_set_bot_solo_owner(bsdr_app *a, bool on) {
    if (!a) return;
    bsdr_mutex_lock(a->lock); a->bot_solo_owner = on; bsdr_mutex_unlock(a->lock);   /* botaudio reads live */
    settings_save(a);
    BSDR_INFO("bsdr.app", "bot loopback solo-owner (listen only to me) -> %s", on ? "on" : "off");
}

/* Follow-me: poll the operator's current room and, when it changes, move the bot with them (leave the
 * old room, join the new). Called on a timer from the main loop. Cheap no-op when follow is off or the
 * bot isn't a live participant. */
void bsdr_app_bot_follow_tick(bsdr_app *a) {
    if (!a) return;
    char host_tok[2048], cur[128];
    bsdr_mutex_lock(a->lock);
    int active = a->bot_follow && a->cloud_logged_in && a->bot_logged_in && !a->bot_stopped;
    snprintf(host_tok, sizeof host_tok, "%s", a->access_token);
    snprintf(cur, sizeof cur, "%s", a->bot_room_id);   /* room the bot is currently in */
    bsdr_mutex_unlock(a->lock);
    if (!active || !host_tok[0]) return;

    char op_room[128] = "";
    if (!bsdr_cloud_poll_room_id(host_tok, op_room, sizeof op_room)) {
        /* Operator left every room -> pull the bot out too (if it was in one). */
        if (cur[0] && a->bot_joined) {
            BSDR_INFO("bsdr.app", "follow-me: operator left; the bot leaves %s", cur);
            bsdr_app_bot_leave_room(a);
        }
        return;
    }
    /* Same room (bare-vs-prefixed tolerant): nothing to do. */
    const char *c = cur, *o = op_room;
    if (strncmp(c, "room:", 5) == 0) c += 5;
    if (strncmp(o, "room:", 5) == 0) o += 5;
    if (a->bot_joined && strcmp(c, o) == 0) return;

    BSDR_INFO("bsdr.app", "follow-me: operator moved to %s (bot was in %s) — following", op_room,
              cur[0] ? cur : "(none)");
    if (cur[0] && a->bot_joined) bsdr_app_bot_leave_room(a);   /* drop the old room first */
    bsdr_app_bot_join_room(a);                                 /* re-resolves + joins the operator's room */
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
    if (!bsdr_cloud_renew(bsdr_cloud_api_key(), refresh, &rr)) { BSDR_WARN("bsdr.app", "cloud token renew failed"); return false; }
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

void bsdr_app_set_sniff(bsdr_app *a, bool want, const char *password) {
    bsdr_mutex_lock(a->lock);
    a->sniff_want = want;
    a->sniff_dirty = true;   /* capture method comes from sniff_method (set separately) */
    if (password) snprintf(a->sniff_password, sizeof a->sniff_password, "%s", password);
    else a->sniff_password[0] = 0;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);   /* remember the mic on/off across restarts (password is never persisted) */
}
/* Capture method: 0 = passive sniff, 1 = MITM (ARP), 2 = router relay. Marks the sniffer dirty so a
 * running sniffer reconfigures on the next reconcile. `sniff_mitm` is the derived flag the reconcile
 * and the sniffer restart-on-mode-change logic read. */
void bsdr_app_set_sniff_method(bsdr_app *a, int method) {
    bsdr_mutex_lock(a->lock);
    if (method < 0 || method > 2) method = 0;
#if defined(__ANDROID__)
    method = 2;   /* Android can't sniff or MITM locally — force the router relay */
#endif
    a->sniff_method = method;
    a->sniff_mitm = (method == 1);
    a->sniff_dirty = true;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);   /* remember the capture strategy (sniff/mitm/relay) across restarts */
}
int bsdr_app_get_sniff_method(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    int m = a->sniff_method;
    bsdr_mutex_unlock(a->lock);
    return m;
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
    char esc_botname[160], esc_botemail[160], esc_botmsg[200], esc_botroom[200];
    bsdr_json_escape(esc_name, sizeof(esc_name), a->cloud_name);
    bsdr_json_escape(esc_email, sizeof(esc_email), a->cloud_email);
    bsdr_json_escape(esc_msg, sizeof(esc_msg), a->cloud_msg);
    bsdr_json_escape(esc_botname, sizeof(esc_botname), a->bot_name);
    bsdr_json_escape(esc_botemail, sizeof(esc_botemail), a->bot_email);
    bsdr_json_escape(esc_botmsg, sizeof(esc_botmsg), a->bot_msg);
    bsdr_json_escape(esc_botroom, sizeof(esc_botroom), a->bot_room_id);
    bsdr_json_escape(esc_sniff, sizeof(esc_sniff), a->sniff_msg);
    bsdr_json_escape(esc_cc, sizeof(esc_cc), a->compctl_msg);
    bsdr_json_escape(esc_ai, sizeof(esc_ai), a->threed_ai_cmd);
    if (a->sniff_wifi < 0)   /* resolve once, then cache (default iface rarely changes) */
        a->sniff_wifi = bsdr_micsniff_default_wireless() ? 1 : 0;
    int n = snprintf(out, cap,
        "{\"cloud\":{\"loggedIn\":%s,\"email\":\"%s\",\"name\":\"%s\",\"msg\":\"%s\","
        "\"internetSharing\":%s},"
        "\"bot\":{\"loggedIn\":%s,\"email\":\"%s\",\"name\":\"%s\",\"msg\":\"%s\",\"joined\":%s,\"room\":\"%s\",\"mode\":\"%s\",\"stopped\":%s,\"follow\":%s,\"loopback\":%s,\"solo\":%s},"
        "\"quest\":{\"paired\":%s,\"name\":\"%s\",\"ip\":\"%s\",\"streaming\":%s,\"paused\":%s},"
        "\"source\":{\"mode\":\"%s\",\"path\":\"%s\",\"path2\":\"%s\",\"audio\":%s},"
        "\"blank\":%s,\"pointerTouch\":%s,\"cloudMic\":%s,\"ownerMicLocal\":%s,\"roomMic\":%s,\"tlsInsecure\":%s,"
        "\"threed\":{\"mode\":%d,\"deepness\":%d,\"convergence\":%d,\"swap\":%s,\"full\":%s,\"tier\":%d,\"ai\":\"%s\"},"
        "\"quality\":{\"w\":%d,\"h\":%d,\"bitrate\":%d,\"brOverride\":%d,\"gpuEncode\":%s,\"encLevel\":%d,\"x264Threads\":%d,\"vaapi\":%s,\"kmsgrab\":%s,\"lan1x\":%s,\"fpsCap\":%d,\"wifiOpt\":%s},"
        "\"voice\":{\"stt\":\"%s\",\"sttModel\":\"%s\",\"sttToken\":%s,"
        "\"llm\":\"%s\",\"llmModel\":\"%s\",\"llmToken\":%s},"
        "\"sniff\":{\"want\":%s,\"mitm\":%s,\"method\":%d,\"active\":%s,\"msg\":\"%s\",\"wifi\":%s,"
        "\"fxOn\":%s,\"gender\":%d,\"formant\":%d,\"volume\":%d,\"robot\":%d,\"echo\":%d,\"whisper\":%d,\"substitute\":%s,\"relayPort\":%d},"
        "\"compctl\":{\"want\":%s,\"vision\":%s,\"active\":%s,\"msg\":\"%s\"},"
        "\"android\":%s,\"selected\":\"%s\",\"quests\":[",
        a->cloud_logged_in ? "true" : "false", esc_email, esc_name, esc_msg,
        a->internet_sharing ? "true" : "false",
        a->bot_logged_in ? "true" : "false", esc_botemail, esc_botname, esc_botmsg,
        a->bot_joined ? "true" : "false", esc_botroom, a->bot_mode[0] ? a->bot_mode : "audio",
        a->bot_stopped ? "true" : "false", a->bot_follow ? "true" : "false",
        a->bot_loopback ? "true" : "false", a->bot_solo_owner ? "true" : "false",
        a->quest_paired ? "true" : "false", a->quest_name, a->quest_ip,
        a->streaming ? "true" : "false", a->paused ? "true" : "false",
        a->source, a->source_path, a->source_path2, a->audio ? "true" : "false",
        a->blank_want ? "true" : "false", a->pointer_touch ? "true" : "false", a->cloud_mic_fallback ? "true" : "false",
        a->owner_mic_local ? "true" : "false", a->room_mic_want ? "true" : "false",
        bsdr_tls_is_insecure() ? "true" : "false",
        a->threed_mode, a->threed_deepness, a->threed_convergence,
        a->threed_swap ? "true" : "false", a->threed_full ? "true" : "false", a->threed_tier, esc_ai,
        a->res_w, a->res_h, a->bitrate, a->bitrate_override, a->cpu_only ? "false" : "true",
        a->enc_level, a->enc_x264_threads, a->use_vaapi ? "true" : "false", a->use_kmsgrab ? "true" : "false",
        a->lan_1x ? "true" : "false", a->fps_cap,
        a->wifi_opt ? "true" : "false",
        a->stt_endpoint, a->stt_model, a->stt_token[0] ? "true" : "false",
        a->llm_endpoint, a->llm_model, a->llm_token[0] ? "true" : "false",
        a->sniff_want ? "true" : "false", a->sniff_mitm ? "true" : "false", a->sniff_method,
        a->sniff_active ? "true" : "false", esc_sniff, a->sniff_wifi > 0 ? "true" : "false",
        a->voice_fx_on ? "true" : "false",
        a->voice_gender, a->voice_formant, a->voice_volume, a->voice_robot, a->voice_echo, a->voice_whisper,
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
                      ",\"faceswap\":{\"on\":%s,\"tier\":%d,\"source\":\"%s\",\"status\":\"%s\"",
                      a->faceswap_on ? "true" : "false", a->faceswap_tier, esrc, efst);
        /* face-swap model manager: dir, per-file present state, and any in-flight download.
         * Presence is cached (P6.6): re-stat only on a faceswap_gen bump, while a download is active,
         * or ~2 s max — instead of a stat/mkdir storm every 1 s poll. The dir path is computed once,
         * read-only (never mkdir on a status read). */
        bsdr_model_dl fdl; bsdr_faceswap_download_state(&fdl);
        if (!a->fs_dir_cached) { bsdr_faceswap_model_dir_ro(a->fs_dir_cache, sizeof a->fs_dir_cache); a->fs_dir_cached = 1; }
        uint64_t now_ms = bsdr_now_ms();
        if (!a->fs_present_valid || a->fs_present_gen != a->faceswap_gen || fdl.active ||
            now_ms - a->fs_present_ms > 2000) {
            int ready = 1;
            for (int i = 0; i < BSDR_FACESWAP_NFILES; i++) {
                a->fs_present_cache[i] = bsdr_faceswap_file_present(bsdr_faceswap_files[i]);
                if (!a->fs_present_cache[i]) ready = 0;
            }
            a->fs_ready_cache = ready;
            a->fs_present_gen = a->faceswap_gen; a->fs_present_ms = now_ms; a->fs_present_valid = 1;
        }
        char efsd[930]; bsdr_json_escape(efsd, sizeof efsd, a->fs_dir_cache);
        char efderr[160]; bsdr_json_escape(efderr, sizeof efderr, fdl.err);
        n += snprintf(out + n, cap - n, ",\"models\":{\"dir\":\"%s\",\"ready\":%s,\"files\":[",
                      efsd, a->fs_ready_cache ? "true" : "false");
        for (int i = 0; i < BSDR_FACESWAP_NFILES; i++)
            n += snprintf(out + n, cap - n, "%s{\"name\":\"%s\",\"present\":%s}", i ? "," : "",
                          bsdr_faceswap_files[i],
                          a->fs_present_cache[i] ? "true" : "false");
        n += snprintf(out + n, cap - n,
                      "],\"dl\":{\"active\":%s,\"pct\":%d,\"done\":%ld,\"total\":%ld,\"ok\":%s,\"name\":\"%s\",\"err\":\"%s\"}}}",
                      fdl.active ? "true" : "false", fdl.pct, fdl.done, fdl.total,
                      fdl.ok ? "true" : "false", fdl.name, efderr);
    }
    /* AI voice changer: state + engine base models + the voice library + named presets. */
    {
        char evst[160]; bsdr_json_escape(evst, sizeof evst, a->voiceai_status);
        char evv[130];  bsdr_json_escape(evv, sizeof evv, a->voiceai_voice);
        bsdr_model_dl vdl; bsdr_voice_download_state(&vdl);
        char evderr[160]; bsdr_json_escape(evderr, sizeof evderr, vdl.err);
        n += snprintf(out + n, cap - n,
                      ",\"voiceai\":{\"on\":%s,\"tier\":%d,\"voice\":\"%s\",\"key\":%d,\"status\":\"%s\","
                      "\"available\":%s,\"baseReady\":%s,\"rmvpe\":%s,"
                      "\"dl\":{\"active\":%s,\"pct\":%d,\"done\":%ld,\"total\":%ld,\"ok\":%s,\"name\":\"%s\",\"err\":\"%s\"},\"voices\":[",
                      a->voiceai_on ? "true" : "false", a->voiceai_tier, evv, a->voiceai_key, evst,
                      bsdr_voiceai_available() ? "true" : "false",
                      bsdr_voice_base_present(BSDR_VBASE_CONTENT) ? "true" : "false",
                      bsdr_voice_base_present(BSDR_VBASE_RMVPE) ? "true" : "false",
                      vdl.active ? "true" : "false", vdl.pct, vdl.done, vdl.total,
                      vdl.ok ? "true" : "false", vdl.name, evderr);
        bsdr_voice_entry vl[32]; int nv = bsdr_voice_list(vl, 32);
        for (int i = 0; i < nv; i++) {
            char enm[200]; bsdr_json_escape(enm, sizeof enm, vl[i].name);
            n += snprintf(out + n, cap - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"sr\":%d}",
                          i ? "," : "", vl[i].id, enm, vl[i].sample_rate);
        }
        n += snprintf(out + n, cap - n, "],\"presets\":[");
        for (int i = 0; i < 5; i++) {
            char enm[80]; bsdr_json_escape(enm, sizeof enm, a->voice_presets[i].name);
            n += snprintf(out + n, cap - n, "%s{\"name\":\"%s\",\"ai\":%s,\"voice\":\"%s\"}", i ? "," : "",
                          enm, a->voice_presets[i].ai_on ? "true" : "false", a->voice_presets[i].voice);
        }
        n += snprintf(out + n, cap - n, "]}");
    }
    n += snprintf(out + n, cap - n, "}");
    bsdr_mutex_unlock(a->lock);
    return (size_t)n;
}
