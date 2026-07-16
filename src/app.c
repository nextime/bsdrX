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
#include "bsdr/compcontrol.h"   /* bsdr_cc_type/key/click/scroll for the input host services */
#include "bsdr/cloud.h"
#include "bsdr/cloud_stream.h"
#include "bsdr/botroom.h"
#include "bsdr/botaudio.h"
#include "bsdr/botmic.h"
#include "bsdr/plugin.h"
#include "bsdr/roomcmd.h"
#include "bsdr/botsense.h"
#include "bsdr/audio.h"
#include "bsdr/tts.h"
#include "bsdr/stt.h"
#include "bsdr/llm.h"
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

/* One queued utterance (FIFO node behind bsdr_app.tts_q_head/tail; drained by the speak worker). */
struct tts_utt { char text[512]; struct tts_utt *next; };

void bsdr_app_init(bsdr_app *a) {
    memset(a, 0, sizeof(*a));
    a->lock = bsdr_mutex_new();
    a->acl = bsdr_acl_new();          /* tiered voice access-control (owner/host/friend) */
    snprintf(a->source, sizeof(a->source), "desktop");
    snprintf(a->term_backend, sizeof(a->term_backend), "pty");   /* headless-native default */
    a->term_cols = 120; a->term_rows = 36;
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
    a->gain_owner = 1.0f;         /* volume policy defaults: owner loudest, host/friend slightly lower */
    a->gain_guest = 0.7f;
    a->overload_threshold = 3;    /* >3 queued room commands => owner-only until drained */
    a->llm_compact_pct = 80;      /* compaction on by default at 80% of the context window */
    a->llm_compact_strategy = 1;  /* BSDR_COMPACT_SUMMARY — preserve the gist over a long session */
    /* A free, keyless default search endpoint (DuckDuckGo Instant Answer API, JSON). The query is
     * appended as &q=... The owner can point this at any endpoint that takes ?q= / &q=. */
    snprintf(a->web_search_endpoint, sizeof a->web_search_endpoint,
             "https://api.duckduckgo.com/?format=json&no_html=1&no_redirect=1");
    snprintf(a->cdp_endpoint, sizeof a->cdp_endpoint, "http://127.0.0.1:9222");   /* default CDP endpoint (disabled until enabled) */
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

void bsdr_app_await_device_begin(bsdr_app *a) {
    bsdr_mutex_lock(a->lock); a->lan_await_device = 1; bsdr_mutex_unlock(a->lock);
}
bool bsdr_app_await_device_pending(bsdr_app *a) {
    bsdr_mutex_lock(a->lock); int p = a->lan_await_device; bsdr_mutex_unlock(a->lock); return p != 0;
}

void bsdr_app_set_quality(bsdr_app *a, int w, int h, int bitrate) {
    bsdr_mutex_lock(a->lock);
    if (h >= 0) a->lan_await_device = 0;   /* the headset's /device resolution arrived — stop waiting */
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
    bsdr_app_detect_llm_context(a);   /* refresh the auto context-window in the background */
}

/* Background worker: ask the LLM endpoint for the model's context window and cache it. Best-effort. */
static void detect_ctx_fn(void *arg) {
    bsdr_app *a = (bsdr_app *)arg;
    bsdr_llm_config cfg; memset(&cfg, 0, sizeof cfg);
    bsdr_mutex_lock(a->lock);
    snprintf(cfg.endpoint, sizeof cfg.endpoint, "%s", a->llm_endpoint);
    snprintf(cfg.token,    sizeof cfg.token,    "%s", a->llm_token);
    snprintf(cfg.model,    sizeof cfg.model,    "%s", a->llm_model);
    bsdr_mutex_unlock(a->lock);
    if (!cfg.endpoint[0]) return;
    int ctx = bsdr_llm_detect_context(&cfg);
    if (ctx > 0) { bsdr_mutex_lock(a->lock); a->llm_context_detected = ctx; bsdr_mutex_unlock(a->lock); }
}

void bsdr_app_detect_llm_context(bsdr_app *a) {
    if (!a || !a->llm_endpoint[0]) return;
    bsdr_thread_start_detached(detect_ctx_fn, a);   /* don't block the caller / web request */
}

void bsdr_app_request_keyframe(bsdr_app *a) {
    if (a) a->keyframe_gen++;   /* each encode loop serves the new generation once */
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
    /* Stop the speak worker: flag + wake it, then join (it aborts any in-flight utterance). */
    bsdr_thread *w = NULL;
    if (a->lock) {
        bsdr_mutex_lock(a->lock);
        a->tts_stop = true;
        if (a->tts_cv) bsdr_cond_signal(a->tts_cv);
        w = (bsdr_thread *)a->tts_worker; a->tts_worker = NULL;
        bsdr_mutex_unlock(a->lock);
    }
    if (w) bsdr_thread_join(w);
    struct tts_utt *u = (struct tts_utt *)a->tts_q_head;   /* drain any leftover queued lines */
    while (u) { struct tts_utt *n = u->next; free(u); u = n; }
    a->tts_q_head = a->tts_q_tail = NULL;
    if (a->tts_cv) { bsdr_cond_free(a->tts_cv); a->tts_cv = NULL; }
    if (a->botsense) { bsdr_botsense_free((bsdr_botsense *)a->botsense); a->botsense = NULL; }
    if (a->input_inj) { bsdr_injector_destroy((bsdr_injector *)a->input_inj); a->input_inj = NULL; }
    if (a->acl) { bsdr_acl_free(a->acl); a->acl = NULL; }
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

/* Grace window (ms) between an unpair / heartbeat loss and actually tearing the relay stream down, so a
 * quick pair-cycle doesn't drop the internet share (and remote viewers don't have to re-share). */
#define BSDR_UNPAIR_GRACE_MS 10000

void bsdr_app_set_paired(bsdr_app *a, bool paired, const char *name, const char *ip) {
    bsdr_mutex_lock(a->lock);
    a->quest_paired = paired;
    if (paired) {
        snprintf(a->quest_name, sizeof(a->quest_name), "%s", name ? name : "");
        snprintf(a->quest_ip, sizeof(a->quest_ip), "%s", ip ? ip : "");
        /* re-pair within the grace window: cancel any pending relay teardown, keep the stream */
        if (a->unpair_pending) {
            a->unpair_pending = false; a->unpair_deadline_ms = 0;
            BSDR_INFO("bsdr.app", "re-paired within grace — keeping the internet-share stream");
        }
    } else {
        a->quest_name[0] = a->quest_ip[0] = '\0';
        a->streaming = false;
        /* Do NOT stop the relay here — arm the grace timer instead (finalized by
         * bsdr_app_unpair_grace_expired). internet_sharing stays set so a re-pair resumes seamlessly. */
        if ((a->cloud_stream || a->internet_sharing) && !a->unpair_pending) {
            a->unpair_pending = true;
            a->unpair_deadline_ms = bsdr_now_ms() + BSDR_UNPAIR_GRACE_MS;
            BSDR_INFO("bsdr.app", "unpair/heartbeat loss — internet share held for %d ms pending a re-pair",
                      BSDR_UNPAIR_GRACE_MS);
        }
    }
    bsdr_mutex_unlock(a->lock);
}

/* Finalize the teardown (stop the relay, clear sharing + the pending flag). Lock must be held; returns
 * the stream to stop OUTSIDE the lock (bsdr_cloud_stream_stop can block). */
static bsdr_cloud_stream *unpair_finalize_locked(bsdr_app *a) {
    bsdr_cloud_stream *to_stop = a->cloud_stream;
    a->cloud_stream = NULL;
    a->internet_sharing = false;
    a->unpair_pending = false;
    a->unpair_deadline_ms = 0;
    return to_stop;
}

bool bsdr_app_unpair_grace_expired(bsdr_app *a) {
    bsdr_cloud_stream *to_stop = NULL; bool fired = false;
    bsdr_mutex_lock(a->lock);
    if (a->unpair_pending && !a->quest_paired && bsdr_now_ms() >= a->unpair_deadline_ms) {
        to_stop = unpair_finalize_locked(a);
        fired = true;
    }
    bsdr_mutex_unlock(a->lock);
    if (fired) BSDR_INFO("bsdr.app", "unpair grace expired (no re-pair) — stopping the internet-share stream");
    if (to_stop) bsdr_cloud_stream_stop(to_stop);
    return fired;
}

void bsdr_app_unpair_now(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bsdr_cloud_stream *to_stop = unpair_finalize_locked(a);
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

void bsdr_app_recapture(bsdr_app *a) {
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    a->threed_gen++;   /* same reopen path the 3D toggle uses: close+reopen re-queries the video-src hook */
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
    settings_save(a);   /* remember the owner-mic source choice across restarts */
}

void bsdr_app_set_owner_mic_to_questmic(bsdr_app *a, bool on) {
    bsdr_mutex_lock(a->lock);
    a->owner_mic_to_questmic = on;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);   /* the "route owner voice into BSDR_QuestMic" toggle must survive a restart */
    BSDR_INFO("bsdr.app", "owner voice -> BSDR_QuestMic device: %s", on ? "on" : "off");
}

void bsdr_app_set_tts(bsdr_app *a, const bsdr_tts_config *cfg, bool enabled, int route) {
    bsdr_mutex_lock(a->lock);
    if (cfg) {
        char keep[256]; snprintf(keep, sizeof keep, "%s", a->tts.token);   /* preserve secret when blank */
        a->tts = *cfg;
        if (!a->tts.token[0]) snprintf(a->tts.token, sizeof a->tts.token, "%s", keep);
    }
    a->tts_enabled = enabled;
    a->tts_route = route ? 1 : 0;
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "TTS %s: engine=%s route=%s", enabled ? "on" : "off",
              (cfg && cfg->engine == BSDR_TTS_CLOUD) ? "cloud" : "local",
              route ? "desktop-audio" : "cloud-room");
}

int bsdr_app_botmic_push(bsdr_app *a, const int16_t *pcm, int frames) {
    /* Push under a->lock: bot_mic is only ever freed (in leave/stop) after being nulled under the
     * same lock, so holding it here for the brief encode+send makes the free wait — no use-after-free. */
    int rc = -1;
    bsdr_mutex_lock(a->lock);
    if (a->bot_mic) rc = bsdr_botmic_push((bsdr_botmic *)a->bot_mic, pcm, frames);
    bsdr_mutex_unlock(a->lock);
    return rc;
}

/* Synthesize one utterance and pace it out — to the cloud room mic (route 0) or, played into the
 * desktop sink, the Quest (route 1). Runs on the speak worker, off the caller's thread. Aborts
 * mid-utterance if teardown flips a->tts_stop (plain read is fine for a cooperative cancel flag). */
static void tts_render_one(bsdr_app *a, const char *text) {
    bsdr_tts_config cfg; bool enabled; int route;
    bsdr_mutex_lock(a->lock);
    cfg = a->tts; enabled = a->tts_enabled; route = a->tts_route;
    bsdr_mutex_unlock(a->lock);
    if (!enabled) return;

    int16_t *pcm = NULL;
    int frames = bsdr_tts_synth(&cfg, text, &pcm);       /* 48 kHz mono */
    if (frames <= 0 || !pcm) return;

    if (route == 1) {
#ifdef BSDR_HAVE_AUDIO
        /* Desktop audio: play into the default sink (bsdr_speaker) so it's captured and reaches the
         * Quest alongside the desktop audio. The player's own buffer paces playout. */
        bsdr_audio_player *pl = bsdr_audio_player_new_quiet("bsdr_speaker", 1);
        if (pl) {
            for (int off = 0; off < frames && !a->tts_stop; off += 960)
                bsdr_audio_player_push(pl, pcm + off, frames - off < 960 ? frames - off : 960);
            if (!a->tts_stop) bsdr_sleep_ms((frames * 1000) / 48000 + 120);   /* drain before close */
            bsdr_audio_player_free(pl);
        }
#endif
    } else {
        /* Cloud room mic: pace 10 ms (480-sample) RTP frames in realtime — matches the real client's
         * BigMicStream framing — so the receiver's jitter buffer plays out cleanly instead of
         * overflowing on a burst. (botmic dup-sends each frame; the keepalive uses the same 480.) */
        for (int off = 0; off < frames && !a->tts_stop; off += 480) {
            int n = frames - off < 480 ? frames - off : 480;
            bsdr_app_botmic_push(a, pcm + off, n);
            bsdr_sleep_ms(10);
        }
    }
    free(pcm);
}

/* Speak worker: drains the FIFO one utterance at a time so room audio never overlaps. */
static void tts_worker_fn(void *arg) {
    bsdr_app *a = (bsdr_app *)arg;
    bsdr_mutex_lock(a->lock);
    while (!a->tts_stop) {
        struct tts_utt *u = (struct tts_utt *)a->tts_q_head;
        if (!u) { bsdr_cond_wait(a->tts_cv, a->lock); continue; }
        a->tts_q_head = u->next;
        if (!a->tts_q_head) a->tts_q_tail = NULL;
        a->tts_q_len--;
        bsdr_mutex_unlock(a->lock);
        tts_render_one(a, u->text);       /* blocking pace, but off the caller's thread */
        free(u);
        bsdr_mutex_lock(a->lock);
    }
    bsdr_mutex_unlock(a->lock);
}

/* Queue text for the bot to speak and return immediately (never blocks the LLM tool / web handler).
 * The worker (lazily started here) synthesizes + paces it into the room in order. */
void bsdr_app_tts_say(bsdr_app *a, const char *text) {
    if (!a || !text || !text[0]) return;
    bsdr_mutex_lock(a->lock);
    if (!a->tts_enabled || a->tts_stop) { bsdr_mutex_unlock(a->lock); return; }
    if (!a->tts_worker) {                              /* lazy: one idle thread only once TTS is used */
        if (!a->tts_cv) a->tts_cv = bsdr_cond_new();
        a->tts_worker = bsdr_thread_start(tts_worker_fn, a);
    }
    /* bounded FIFO: if we're badly backed up (LLM spamming speak), drop the oldest queued line so
     * fresh speech stays responsive rather than growing an unbounded backlog. */
    if (a->tts_q_len >= 8) {
        struct tts_utt *old = (struct tts_utt *)a->tts_q_head;
        if (old) {
            a->tts_q_head = old->next;
            if (!a->tts_q_head) a->tts_q_tail = NULL;
            a->tts_q_len--;
            free(old);
        }
    }
    struct tts_utt *u = calloc(1, sizeof *u);
    if (u) {
        snprintf(u->text, sizeof u->text, "%s", text);
        if (a->tts_q_tail) ((struct tts_utt *)a->tts_q_tail)->next = u;
        else               a->tts_q_head = u;
        a->tts_q_tail = u;
        a->tts_q_len++;
        if (a->tts_cv) bsdr_cond_signal(a->tts_cv);
    }
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
/* Tiered access-control store (friends/bans/toggles) — a JSON file alongside `settings`, since a
 * variable-length friends list doesn't fit the flat key=value format. */
static bool access_path(char *out, size_t cap) {
    char dir[512];
    if (!config_dir(dir, sizeof(dir))) return false;
    snprintf(out, cap, "%s/access.json", dir);
    return true;
}
void bsdr_app_acl_save(bsdr_app *a) {
    char path[600];
    if (a->acl && access_path(path, sizeof path)) bsdr_acl_save(a->acl, path);
}
static void settings_save(bsdr_app *a);
void bsdr_app_save_settings(bsdr_app *a) { if (a) settings_save(a); }
void bsdr_app_acl_load(bsdr_app *a) {
    char path[600];
    if (a->acl && access_path(path, sizeof path)) bsdr_acl_load(a->acl, path);
}

/* Refresh the room participant roster from a raw /room JSON body (called on join and on room-data
 * pushes). Robust to an unparsable body (leaves the roster empty). Guarded by a->lock. */
void bsdr_app_refresh_roster(bsdr_app *a, const char *room_json) {
    if (!a) return;
    bsdr_roster tmp;
    char self[200];
    bsdr_mutex_lock(a->lock);
    snprintf(self, sizeof self, "%s", a->bot_session_id);
    bsdr_mutex_unlock(a->lock);
    int n = bsdr_roster_parse(&tmp, room_json, self);
    bsdr_mutex_lock(a->lock);
    a->roster = tmp;
    char tok[2048], room[128];
    snprintf(tok, sizeof tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    bool joiner = (n > a->roster_prev_n);          /* someone new appeared since last refresh */
    bool sharing = a->internet_sharing;
    a->roster_prev_n = n;
    bsdr_mutex_unlock(a->lock);
    BSDR_DEBUG("bsdr.app", "roster refreshed: %d participant(s)", n);
    /* A new joiner's cloud consumer needs a keyframe to start decoding; force one now instead of
     * making them wait for the next scheduled GOP (only meaningful while internet-sharing). */
    if (joiner && sharing) { BSDR_INFO("bsdr.app", "new joiner -> forcing a keyframe for the cloud stream"); bsdr_app_request_keyframe(a); }
    /* Soft-ban enforcement: auto-re-kick any banned participant that (re)joined while the bot is here. */
    if (room[0] && tok[0])
        for (int i = 0; i < tmp.n; i++) {
            if (tmp.e[i].is_self || !tmp.e[i].user_session_id[0]) continue;
            if (bsdr_acl_is_banned(a->acl, tmp.e[i].social_id, tmp.e[i].username)) {
                BSDR_INFO("bsdr.app", "re-kicking banned %s", tmp.e[i].username[0] ? tmp.e[i].username : "?");
                bsdr_cloud_kick(tok, room, tmp.e[i].user_session_id);
            }
        }
    /* Auto mic-check: when enabled and the bot is moderating (owner present or a stay-with target),
     * age-verify the first unknown (non-owner/friend/host, non-banned) joiner — one at a time; the
     * router dedups so a given person is only auto-checked once per session. */
    bool mca; char stayw[64];
    bsdr_mutex_lock(a->lock); mca = a->mic_check_auto; snprintf(stayw, sizeof stayw, "%s", a->stay_with); bsdr_mutex_unlock(a->lock);
    if (mca && a->roomcmd && (bsdr_app_owner_ssrc(a) != 0 || stayw[0]))
        for (int i = 0; i < tmp.n; i++) {
            if (tmp.e[i].is_self || tmp.e[i].ssrc == 0) continue;
            if (bsdr_acl_is_banned(a->acl, tmp.e[i].social_id, tmp.e[i].username)) continue;
            if (bsdr_acl_resolve(a->acl, tmp.e[i].social_id, tmp.e[i].username, tmp.e[i].is_host) != BSDR_ACL_NONE)
                continue;   /* owner / friend / host = known */
            if (bsdr_roomcmd_autocheck((bsdr_roomcmd *)a->roomcmd, tmp.e[i].ssrc, tmp.e[i].username)) break;
        }
}

uint32_t bsdr_app_owner_ssrc(bsdr_app *a) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    bsdr_roster s = a->roster;
    bsdr_mutex_unlock(a->lock);
    for (int i = 0; i < s.n; i++) {
        if (s.e[i].is_self) continue;
        if (bsdr_acl_resolve(a->acl, s.e[i].social_id, s.e[i].username, s.e[i].is_host) == BSDR_ACL_OWNER)
            return s.e[i].ssrc;
    }
    return 0;
}

/* Serialize the cached roster as a JSON array for a plugin (bot host-service surface, ABI 4). */
size_t bsdr_app_roster_json(bsdr_app *a, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!a) { if (cap >= 3) { snprintf(out, cap, "[]"); return 2; } return 0; }
    bsdr_mutex_lock(a->lock);
    bsdr_roster s = a->roster;
    bsdr_mutex_unlock(a->lock);
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < s.n && o < cap - 320; i++) {
        int lvl = a->acl ? (int)bsdr_acl_resolve(a->acl, s.e[i].social_id, s.e[i].username, s.e[i].is_host) : 0;
        char ue[160], se[200], le[160];
        bsdr_json_escape(ue, sizeof ue, s.e[i].username);
        bsdr_json_escape(se, sizeof se, s.e[i].social_id);
        bsdr_json_escape(le, sizeof le, s.e[i].legacy_user_id);
        o += (size_t)snprintf(out + o, cap - o,
            "%s{\"ssrc\":%u,\"username\":\"%s\",\"socialId\":\"%s\",\"legacyUserId\":\"%s\","
            "\"level\":%d,\"isHost\":%s,\"isSelf\":%s}",
            i ? "," : "", s.e[i].ssrc, ue, se, le, lvl,
            s.e[i].is_host ? "true" : "false", s.e[i].is_self ? "true" : "false");
    }
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

/* Resolve a speaker's SSRC to {socialId,username,level,isHost,isSelf}; returns 1 if found, else 0. */
int bsdr_app_resolve_ssrc(bsdr_app *a, uint32_t ssrc, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!a || !out || cap == 0) return 0;
    bsdr_mutex_lock(a->lock);
    bsdr_roster s = a->roster;
    bsdr_mutex_unlock(a->lock);
    const bsdr_roster_entry *e = bsdr_roster_by_ssrc(&s, ssrc);
    if (!e) return 0;
    int lvl = a->acl ? (int)bsdr_acl_resolve(a->acl, e->social_id, e->username, e->is_host) : 0;
    char ue[160], se[200];
    bsdr_json_escape(ue, sizeof ue, e->username);
    bsdr_json_escape(se, sizeof se, e->social_id);
    snprintf(out, cap, "{\"socialId\":\"%s\",\"username\":\"%s\",\"level\":%d,\"isHost\":%s,\"isSelf\":%s}",
             se, ue, lvl, e->is_host ? "true" : "false", e->is_self ? "true" : "false");
    return 1;
}

/* Transcribe PCM using the app's configured STT endpoint (bot host-service, ABI 4). Returns 1 on
 * success with text in out, else 0. */
int bsdr_app_stt(bsdr_app *a, const int16_t *pcm, int frames, int rate, int channels,
                 char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!a || !pcm || frames <= 0 || !out || cap == 0) return 0;
    bsdr_stt_config cfg;
    memset(&cfg, 0, sizeof cfg);
    bsdr_mutex_lock(a->lock);
    snprintf(cfg.endpoint, sizeof cfg.endpoint, "%s", a->stt_endpoint);
    snprintf(cfg.token,    sizeof cfg.token,    "%s", a->stt_token);
    snprintf(cfg.model,    sizeof cfg.model,    "%s", a->stt_model);
    bsdr_mutex_unlock(a->lock);
    return bsdr_stt_transcribe(&cfg, pcm, frames, rate, channels, out, cap) ? 1 : 0;
}

/* A bot plugin subscribes/unsubscribes to complete room utterances. Lazily brings up the segmenter on
 * first subscribe; NULL cb tears the subscription down (waiting for any in-flight callback). The
 * segmenter itself is freed with the app. */
void bsdr_app_utterance_subscribe(bsdr_app *a, bsdr_utterance_cb cb, void *user) {
    if (!a) return;
    if (cb && !a->botsense) a->botsense = bsdr_botsense_new();
    if (a->botsense) bsdr_botsense_set_cb((bsdr_botsense *)a->botsense, cb, user);
    if (cb) BSDR_INFO("bsdr.app", "a plugin now owns the bot's hearing (in-core router bypassed)");
    else    BSDR_INFO("bsdr.app", "plugin bot hearing released (in-core router resumes)");
}

int bsdr_app_botsense_active(bsdr_app *a) {
    return (a && a->botsense && bsdr_botsense_has_cb((bsdr_botsense *)a->botsense)) ? 1 : 0;
}

/* One LLM round-trip using the app's configured endpoint (bot host-service, ABI 4). The plugin owns the
 * agentic loop; this is the wire call. Returns 1 with the assistant message object in out, else 0. */
int bsdr_app_llm_complete(bsdr_app *a, const char *messages_json, const char *tools_json,
                          char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!a) return 0;
    bsdr_llm_config cfg;
    memset(&cfg, 0, sizeof cfg);
    bsdr_mutex_lock(a->lock);
    snprintf(cfg.endpoint, sizeof cfg.endpoint, "%s", a->llm_endpoint);
    snprintf(cfg.token,    sizeof cfg.token,    "%s", a->llm_token);
    snprintf(cfg.model,    sizeof cfg.model,    "%s", a->llm_model);
    bsdr_mutex_unlock(a->lock);
    return bsdr_llm_complete_once(&cfg, messages_json, tools_json, out, cap);
}

/* The bot avatar presence state (0=off 1=connecting 2=up 3=ghost) — bot host-service. */
int bsdr_app_avatar_state(bsdr_app *a) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    void *br = a->bot_room;
    bsdr_mutex_unlock(a->lock);
    return br ? (int)bsdr_botroom_avatar_state((bsdr_botroom *)br) : 0;
}

/* The bot's OWN legacyUserId (from its is_self roster entry). Returns 1 and fills out if known. */
int bsdr_app_local_legacy_id(bsdr_app *a, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!a || !out || cap == 0) return 0;
    bsdr_mutex_lock(a->lock);
    bsdr_roster s = a->roster;
    bsdr_mutex_unlock(a->lock);
    for (int i = 0; i < s.n; i++)
        if (s.e[i].is_self && s.e[i].legacy_user_id[0]) { snprintf(out, cap, "%s", s.e[i].legacy_user_id); return 1; }
    return 0;
}

bool bsdr_app_kick_user(bsdr_app *a, const char *username) {
    if (!a || !username || !username[0]) return false;
    char tok[2048], room[128], usid[200] = "";
    bsdr_mutex_lock(a->lock);
    snprintf(tok, sizeof tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    const bsdr_roster_entry *e = bsdr_roster_by_username(&a->roster, username);
    if (e) snprintf(usid, sizeof usid, "%s", e->user_session_id);
    bsdr_mutex_unlock(a->lock);
    if (!usid[0] || !room[0] || !tok[0]) { BSDR_WARN("bsdr.app", "kick: %s not found / no room", username); return false; }
    return bsdr_cloud_kick(tok, room, usid) / 100 == 2;
}

/* Reset the room: kick every participant so everyone drops to the lobby and can rejoin a fresh room.
 * Owner-only. Clean server action (the official kick) — recovers a stuck/frozen room without crashing
 * anyone. Returns the number kicked. */
int bsdr_app_reset_room(bsdr_app *a) {
    if (!a) return 0;
    char tok[2048], room[128], self[200];
    bsdr_roster s;
    bsdr_mutex_lock(a->lock);
    snprintf(tok, sizeof tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    snprintf(self, sizeof self, "%s", a->bot_session_id);
    s = a->roster;
    bsdr_mutex_unlock(a->lock);
    if (!room[0] || !tok[0]) { BSDR_WARN("bsdr.app", "reset_room: bot not in a room"); return 0; }
    int n = 0;
    for (int i = 0; i < s.n; i++) {
        if (s.e[i].is_self || !s.e[i].user_session_id[0]) continue;      /* not the bot itself */
        if (self[0] && strcmp(s.e[i].user_session_id, self) == 0) continue;
        if (bsdr_cloud_kick(tok, room, s.e[i].user_session_id) / 100 == 2) n++;
    }
    BSDR_INFO("bsdr.app", "reset_room: kicked %d participant(s) from %s (everyone rejoins fresh)", n, room);
    return n;
}

bool bsdr_app_ban_user(bsdr_app *a, const char *username) {
    if (!a || !username || !username[0]) return false;
    char sid[80] = "";
    bsdr_mutex_lock(a->lock);
    const bsdr_roster_entry *e = bsdr_roster_by_username(&a->roster, username);
    if (e) snprintf(sid, sizeof sid, "%s", e->social_id);
    bsdr_mutex_unlock(a->lock);
    bsdr_acl_ban_add(a->acl, sid[0] ? sid : NULL, username);   /* persist first, so a rejoin is caught */
    bsdr_app_acl_save(a);
    return bsdr_app_kick_user(a, username);
}

int bsdr_app_audio_gains(bsdr_app *a, uint32_t *ssrc_out, float *gain_out, int cap, float *default_out) {
    if (!a || !ssrc_out || !gain_out || cap <= 0) return -1;
    bsdr_mutex_lock(a->lock);
    bsdr_roster s = a->roster;            /* snapshot */
    bool solo_owner = a->bot_solo_owner;
    uint32_t mc = a->mic_check_ssrc;
    float g_owner = a->gain_owner, g_guest = a->gain_guest;
    bsdr_mutex_unlock(a->lock);
    if (s.n == 0) return -1;
    int m = 0, owner_found = 0;
    for (int i = 0; i < s.n && m < cap; i++) {
        if (s.e[i].is_self || s.e[i].ssrc == 0) continue;   /* never loop the bot's own voice */
        bsdr_acl_level lvl = bsdr_acl_resolve(a->acl, s.e[i].social_id, s.e[i].username, s.e[i].is_host);
        float g;
        if (s.e[i].ssrc == mc)                 g = 1.0f;    /* mic-check target: keep audible */
        else if (lvl == BSDR_ACL_OWNER)      { g = g_owner; owner_found = 1; }
        else if (solo_owner)                   g = 0.0f;    /* "listen only to me" */
        else if (lvl == BSDR_ACL_HOST || lvl == BSDR_ACL_FRIEND) g = g_guest;  /* slightly below owner */
        else                                   g = 0.0f;    /* strangers silenced */
        if (lvl == BSDR_ACL_OWNER) owner_found = 1;
        ssrc_out[m] = s.e[i].ssrc; gain_out[m] = g; m++;
    }
    /* Only mute-by-default once we can actually identify the owner in the room — otherwise a bad/empty
     * roster would silence everyone (incl. the owner). Without that anchor, fall back to unity. */
    if (!owner_found && mc == 0) return -1;
    if (default_out) *default_out = 0.0f;
    return m;
}

static void settings_save(bsdr_app *a) {
    char path[600];
    if (!settings_path(path, sizeof(path))) return;
    bsdr_mutex_lock(a->lock);
    int cpu = a->cpu_only ? 1 : 0, bro = a->bitrate_override, ptouch = a->pointer_touch ? 1 : 0;
    int encp = a->enc_level, lan1x = a->lan_1x ? 1 : 0, fcap = a->fps_cap, wopt = a->wifi_opt ? 1 : 0;
    int x264t = a->enc_x264_threads, vaapi = a->use_vaapi ? 1 : 0, kms = a->use_kmsgrab ? 1 : 0;
    int cpli = a->cloud_rtcp_pli ? 1 : 0;
    int smeth = a->sniff_method, sport = a->sniff_remote_port, swant = a->sniff_want ? 1 : 0;
    int omlocal = a->owner_mic_local ? 1 : 0, omquest = a->owner_mic_to_questmic ? 1 : 0;
    char bmode[16]; snprintf(bmode, sizeof bmode, "%s", a->bot_mode[0] ? a->bot_mode : "audio");
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
    int mca = a->mic_check_auto ? 1 : 0;
    int govr = (int)(a->gain_owner * 100 + 0.5f), ggst = (int)(a->gain_guest * 100 + 0.5f), othr = a->overload_threshold;
    int lctx = a->llm_context_tokens, lcp = a->llm_compact_pct, lcs = a->llm_compact_strategy, lmr = a->llm_max_rounds;
    char wse[256], wst[256];
    snprintf(wse, sizeof wse, "%s", a->web_search_endpoint);
    snprintf(wst, sizeof wst, "%s", a->web_search_token);
    bsdr_mutex_unlock(a->lock);
    FILE *f = fopen(path, "w");
    if (!f) { BSDR_WARN("bsdr.app", "could not save settings to %s", path); return; }
    fprintf(f, "cloud_rtcp_pli=%d\n", cpli);
    fprintf(f, "cpu_only=%d\nbitrate_override=%d\npointer_touch=%d\n"
               "sniff_method=%d\nsniff_relay_port=%d\nsniff_want=%d\nbot_mode=%s\nbot_follow=%d\n"
               "bot_loopback=%d\nbot_solo_owner=%d\nenc_level=%d\nlan_1x=%d\nfps_cap=%d\nwifi_opt=%d\n"
               "x264_threads=%d\nuse_vaapi=%d\nuse_kmsgrab=%d\n",
            cpu, bro, ptouch, smeth, sport, swant, bmode, bfollow, bloop, bsolo, encp, lan1x, fcap, wopt,
            x264t, vaapi, kms);
    fprintf(f, "owner_mic_local=%d\nowner_mic_to_questmic=%d\n", omlocal, omquest);   /* owner-mic routing */
    fprintf(f, "file_loop=%d\n", a->file_loop);   /* loop the file/playlist source continuously */
    fprintf(f, "term_backend=%s\nterm_cols=%d\nterm_rows=%d\n",   /* terminal-source prefs */
            a->term_backend[0] ? a->term_backend : "pty", a->term_cols, a->term_rows);
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
    /* in-room bot moderation prefs */
    fprintf(f, "mic_check_auto=%d\n", mca);
    fprintf(f, "gain_owner=%d\ngain_guest=%d\noverload_threshold=%d\n", govr, ggst, othr);
    fprintf(f, "llm_context_tokens=%d\nllm_compact_pct=%d\nllm_compact_strategy=%d\nllm_max_rounds=%d\n",
            lctx, lcp, lcs, lmr);
    fprintf(f, "browser_ctl=%d\n", a->browser_ctl_enabled ? 1 : 0);
    if (a->cdp_endpoint[0]) fprintf(f, "cdp_endpoint=%s\n", a->cdp_endpoint);
    if (wse[0]) fprintf(f, "web_search_endpoint=%s\n", wse);
    if (wst[0]) fprintf(f, "web_search_token=%s\n", wst);
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
        else if (sscanf(line, "owner_mic_local=%d", &v) == 1)       a->owner_mic_local = v ? true : false;
        else if (sscanf(line, "owner_mic_to_questmic=%d", &v) == 1) a->owner_mic_to_questmic = v ? true : false;
        else if (sscanf(line, "enc_level=%d", &v) == 1)        a->enc_level = v < 0 ? 0 : v > 2 ? 2 : v;
        else if (sscanf(line, "enc_perf=%d", &v) == 1)         a->enc_level = v ? 2 : 0;  /* legacy bool -> level */
        else if (sscanf(line, "x264_threads=%d", &v) == 1)     a->enc_x264_threads = v < 0 ? 0 : v > 32 ? 32 : v;
        else if (sscanf(line, "file_loop=%d", &v) == 1)        a->file_loop = v ? 1 : 0;
        else if (sscanf(line, "term_cols=%d", &v) == 1)        a->term_cols = (v >= 20 && v <= 400) ? v : 120;
        else if (sscanf(line, "term_rows=%d", &v) == 1)        a->term_rows = (v >= 6 && v <= 120) ? v : 36;
        else if (sscanf(line, "term_backend=%7[a-z]", a->term_backend) == 1) { if (strcmp(a->term_backend,"xvfb")) snprintf(a->term_backend, sizeof(a->term_backend), "pty"); }
        else if (sscanf(line, "use_vaapi=%d", &v) == 1)        a->use_vaapi = v ? true : false;
        else if (sscanf(line, "use_kmsgrab=%d", &v) == 1)      a->use_kmsgrab = v ? true : false;
        else if (sscanf(line, "cloud_rtcp_pli=%d", &v) == 1)   a->cloud_rtcp_pli = v ? true : false;
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
        else if (sscanf(line, "bot_mode=%15[^\n]", a->bot_mode) == 1) {
            /* Accept any saved mode name: "audio" is built-in; a plugin mode (e.g. "fullbot") activates
             * when its plugin registers it at load. Legacy "full" maps to the plugin "fullbot". If no
             * plugin ever claims the name it simply behaves as bare audio (nothing activates it). */
            if (strcmp(a->bot_mode, "full") == 0) snprintf(a->bot_mode, sizeof a->bot_mode, "fullbot");
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
        else if (sscanf(line, "mic_check_auto=%d", &v) == 1) a->mic_check_auto = v ? true : false;
        else if (sscanf(line, "gain_owner=%d", &v) == 1) a->gain_owner = v / 100.0f;
        else if (sscanf(line, "gain_guest=%d", &v) == 1) a->gain_guest = v / 100.0f;
        else if (sscanf(line, "overload_threshold=%d", &v) == 1) a->overload_threshold = (v >= 1 && v <= 99) ? v : 3;
        else if (strncmp(line, "bot_personality=", 16) == 0) continue;   /* moved to the fullbot plugin */
        else if (sscanf(line, "browser_ctl=%d", &v) == 1) a->browser_ctl_enabled = v ? true : false;
        else if (sscanf(line, "cdp_endpoint=%255[^\n]", a->cdp_endpoint) == 1) continue;
        else if (sscanf(line, "llm_context_tokens=%d", &v) == 1) a->llm_context_tokens = v >= 0 ? v : 0;
        else if (sscanf(line, "llm_compact_pct=%d", &v) == 1) a->llm_compact_pct = (v <= 100) ? v : 80;
        else if (sscanf(line, "llm_compact_strategy=%d", &v) == 1) a->llm_compact_strategy = (v >= 0 && v <= 2) ? v : 1;
        else if (sscanf(line, "llm_max_rounds=%d", &v) == 1) a->llm_max_rounds = (v >= 0 && v <= 200) ? v : 0;
        else if (sscanf(line, "web_search_endpoint=%255[^\n]", a->web_search_endpoint) == 1) continue;
        else if (sscanf(line, "web_search_token=%255[^\n]", a->web_search_token) == 1) continue;
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
    bsdr_app_acl_load(a);   /* friends / bans / access toggles (access.json) */
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
    char name[128] = "", osid[80] = "";
    if (ok) {
        bsdr_cloud_account(bsdr_cloud_api_key(), res.access_token, name, sizeof(name));
        bsdr_cloud_my_socialid(res.access_token, osid, sizeof osid);   /* owner = the primary account */
    }

    bsdr_mutex_lock(a->lock);
    a->cloud_logged_in = ok;
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), "%.150s", res.message);
    if (ok) {
        snprintf(a->access_token, sizeof(a->access_token), "%s", res.access_token);
        snprintf(a->refresh_token, sizeof(a->refresh_token), "%s", res.refresh_token);
        snprintf(a->cloud_name, sizeof(a->cloud_name), "%s", name[0] ? name : email);
    }
    bsdr_mutex_unlock(a->lock);
    if (ok) { bsdr_acl_set_owner(a->acl, osid, name[0] ? name : email); bsdr_app_acl_save(a); }

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
    int st = bsdr_cloud_account_status(bsdr_cloud_api_key(), access, vname, sizeof(vname));
    bool valid = (st / 100 == 2);
    bool reachable = (st != 0);
    if (!valid && refresh[0]) {
        BSDR_INFO("bsdr.app", "saved token expired — renewing");
        bsdr_cloud_result rr; memset(&rr, 0, sizeof rr);
        if (bsdr_cloud_renew(bsdr_cloud_api_key(), refresh, &rr)) {
            reachable = true;
            snprintf(access, sizeof(access), "%s", rr.access_token);
            if (rr.refresh_token[0]) snprintf(refresh, sizeof(refresh), "%s", rr.refresh_token);
            st = bsdr_cloud_account_status(bsdr_cloud_api_key(), access, vname, sizeof(vname));
            valid = (st / 100 == 2);
        } else {
            reachable = reachable || (rr.http_status != 0);   /* renew reached the server (rejected) */
        }
    }
    if (!valid) {
        /* Only DISCARD the session when the server actually rejected it. A network failure (unreachable)
         * must NOT throw away a token we couldn't even check — keep it and let the live loop re-verify
         * once the cloud is reachable, so a transient blip doesn't log the operator out. */
        if (reachable) { BSDR_INFO("bsdr.app", "saved session invalid; please log in again"); return false; }
        BSDR_WARN("bsdr.app", "cloud unreachable at startup — keeping the saved session (will re-verify when online)");
    }

    bsdr_mutex_lock(a->lock);
    a->cloud_logged_in = true;
    snprintf(a->access_token, sizeof(a->access_token), "%s", access);
    snprintf(a->refresh_token, sizeof(a->refresh_token), "%s", refresh);
    snprintf(a->cloud_email, sizeof(a->cloud_email), "%s", email);
    snprintf(a->cloud_name, sizeof(a->cloud_name), "%s", vname[0] ? vname : (name[0] ? name : email));
    snprintf(a->cloud_msg, sizeof(a->cloud_msg), valid ? "logged in (restored)" : "restored (offline — re-verifying)");
    bsdr_mutex_unlock(a->lock);

    session_save(a);   /* persist any renewed token */
    /* owner = the primary account; resolve its socialId for access-control (best-effort, non-fatal).
     * Skip the network call entirely when access.json already carries it — otherwise we'd re-resolve
     * (and re-log any server error) on every launch even though it never changes. */
    char osid[80] = "";
    bsdr_acl_get_owner_social_id(a->acl, osid, sizeof osid);
    if (!osid[0]) bsdr_cloud_my_socialid(access, osid, sizeof osid);
    bsdr_acl_set_owner(a->acl, osid, a->cloud_name); bsdr_app_acl_save(a);
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
        bsdr_acl_set_bot(a->acl, sid, a->bot_name);   /* bot identity + default wake word (its name) */
        bsdr_app_acl_save(a);
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
    int st = bsdr_cloud_account_status(bsdr_cloud_client_key(), access, vname, sizeof(vname));
    bool valid = (st / 100 == 2);
    bool reachable = (st != 0);
    if (!valid && refresh[0]) {
        bsdr_cloud_result rr; memset(&rr, 0, sizeof rr);
        if (bsdr_cloud_renew(bsdr_cloud_client_key(), refresh, &rr)) {
            reachable = true;
            snprintf(access, sizeof(access), "%s", rr.access_token);
            if (rr.refresh_token[0]) snprintf(refresh, sizeof(refresh), "%s", rr.refresh_token);
            st = bsdr_cloud_account_status(bsdr_cloud_client_key(), access, vname, sizeof(vname));
            valid = (st / 100 == 2);
        } else {
            reachable = reachable || (rr.http_status != 0);
        }
    }
    if (!valid) {
        /* keep the bot session across a transient connect failure (see bsdr_app_restore_session) */
        if (reachable) { BSDR_INFO("bsdr.app", "saved bot session invalid; log the bot in again"); return; }
        BSDR_WARN("bsdr.app", "cloud unreachable — keeping the saved bot session (will re-verify when online)");
    }

    bsdr_mutex_lock(a->lock);
    a->bot_logged_in = true;
    a->bot_stopped = false;
    a->bot_token_ms = bsdr_now_ms();   /* token just verified/renewed — start the proactive-renew clock */
    snprintf(a->bot_access_token, sizeof(a->bot_access_token), "%s", access);
    snprintf(a->bot_refresh_token, sizeof(a->bot_refresh_token), "%s", refresh);
    snprintf(a->bot_email, sizeof(a->bot_email), "%s", email);
    snprintf(a->bot_name, sizeof(a->bot_name), "%s", vname[0] ? vname : (name[0] ? name : email));
    snprintf(a->bot_msg, sizeof(a->bot_msg), "logged in (restored)");
    char bname[128]; snprintf(bname, sizeof bname, "%s", a->bot_name);
    bsdr_mutex_unlock(a->lock);
    /* Restore the bot identity into the ACL too (login does this; restore didn't). set_bot defaults the
     * wake word to the bot's own name when the owner hasn't set one — so "hey <botname>" works out of
     * the box after a restart. socialId is left blank (resolved lazily / matched by username). */
    bsdr_acl_set_bot(a->acl, "", bname);
    bsdr_app_acl_save(a);
    bot_session_save(a);
    bsdr_cloud_ws *ws = bsdr_cloud_ws_open(access, 1);
    bsdr_mutex_lock(a->lock); a->bot_ws = ws; bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "restored bot account %s", vname[0] ? vname : email);
}

static void bot_set_msg(bsdr_app *a, bool joined, const char *room, const char *fmt, ...);
static void bot_loopback_apply(bsdr_app *a);
static bool bot_renew_token(bsdr_app *a);

void bsdr_app_bot_logout(bsdr_app *a) {
    bsdr_mutex_lock(a->lock);
    bsdr_cloud_ws *ws = a->bot_ws;
    bsdr_botroom *room = (bsdr_botroom *)a->bot_room; a->bot_room = NULL;
    bsdr_botaudio *ba = (bsdr_botaudio *)a->bot_audio; a->bot_audio = NULL;
    bsdr_botmic *bm = (bsdr_botmic *)a->bot_mic; a->bot_mic = NULL;
    a->bot_ws = NULL; a->bot_logged_in = false; a->bot_joined = false;
    a->bot_access_token[0] = a->bot_refresh_token[0] = a->bot_name[0] = a->bot_email[0] = a->bot_room_id[0] = 0;
    snprintf(a->bot_msg, sizeof(a->bot_msg), "not logged in");
    bsdr_mutex_unlock(a->lock);
    if (room) bsdr_botroom_stop(room);   /* tear down the avatar presence */
    if (ba) bsdr_botaudio_stop(ba);      /* tear down the cloud-mic loopback */
    if (bm) bsdr_botmic_stop(bm);        /* tear down the room-mic producer */
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
    bsdr_botmic *bm = (bsdr_botmic *)a->bot_mic; a->bot_mic = NULL;
    snprintf(bot_tok, sizeof bot_tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    bsdr_mutex_unlock(a->lock);
    if (br) bsdr_botroom_stop(br);   /* stop broadcasting the avatar first */
    if (ba) bsdr_botaudio_stop(ba);  /* stop the cloud-mic loopback */
    if (bm) bsdr_botmic_stop(bm);    /* stop the room-mic producer */
    /* Clear the joined state up front so the UI/roster stop showing the bot immediately, and the
     * follow/watch ticks don't treat it as still-in-room (the network leave below is best-effort). */
    bsdr_mutex_lock(a->lock);
    a->bot_joined = false;
    a->bot_room_id[0] = 0;
    a->bot_audio_ip[0] = 0; a->bot_audio_port = 0; a->bot_owner_sid[0] = 0;
    bsdr_mutex_unlock(a->lock);
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
    bsdr_botmic *bm = (bsdr_botmic *)a->bot_mic; a->bot_mic = NULL;
    snprintf(bot_tok, sizeof bot_tok, "%s", a->bot_access_token);
    snprintf(room, sizeof room, "%s", a->bot_room_id);
    a->bot_logged_in = false; a->bot_joined = false; a->bot_stopped = true;
    a->bot_room_id[0] = 0;
    snprintf(a->bot_msg, sizeof a->bot_msg, "stopped — login remembered (Start to reconnect)");
    bsdr_mutex_unlock(a->lock);
    if (br) bsdr_botroom_stop(br);
    if (ba) bsdr_botaudio_stop(ba);                      /* stop the cloud-mic loopback */
    if (bm) bsdr_botmic_stop(bm);                        /* stop the room-mic producer */
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
    char host_tok[2048], bot_tok[2048], bot_sid[80], mode[16];
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

    /* 1b) EXPIRED BOT TOKEN. The bot's access token is short-lived and expires mid-session; then the join
     * (and /social/profile) 403 with "requires a valid entry for 'x-access-token'" — which is NOT an
     * authorization refusal. Renew the bot token and retry the direct join ONCE before assuming the bot
     * needs invite staging (the old code mis-read this 403 as "need a RoomInvite"). */
    if (!ok && (peer.http_status == 401 || peer.http_status == 403) && bot_renew_token(a)) {
        bsdr_mutex_lock(a->lock); snprintf(bot_tok, sizeof bot_tok, "%s", a->bot_access_token); bsdr_mutex_unlock(a->lock);
        if (!bot_sid[0]) { bsdr_cloud_my_socialid(bot_tok, bot_sid, sizeof bot_sid);
            bsdr_mutex_lock(a->lock); snprintf(a->bot_social_id, sizeof a->bot_social_id, "%s", bot_sid); bsdr_mutex_unlock(a->lock); }
        memset(&peer, 0, sizeof peer);
        ok = bsdr_cloud_join_room(bot_tok, room.room_id, &peer);
        BSDR_INFO("bsdr.app", "bot join retried after token renew -> %s (HTTP %d)", ok ? "OK" : "still refused", peer.http_status);
    }

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

    /* After a successful join, bring up the bot's room MEDIA. The ROOM-MIC PRODUCER is the piece that
     * matters for the bare "audio" mode: a REST join alone makes the CLOUD participant count rise, but the
     * Quest gates the owner mic on seeing a real co-participant's MEDIA on the SFU — proven in the field, a
     * REST-only bot left participants=2 yet the Quest never resumed transmitting the owner mic. So the
     * producer must run for ANY join, not only when the avatar renders (this used to be gated on
     * mode=="full" since 29169ff, which is why "audio" mode never unlocked the mic). The AVATAR is a
     * separate data-channel presence and stays gated on want_avatar. The bot's own MediaPeer (relay ip +
     * mic/data ports + legacyUserId + seat) comes from GET /room; the flat me.mediaPeer in the join
     * response is often empty ({}) or the SCREEN's, so prefer GET /room. */
    int want_avatar; bsdr_mutex_lock(a->lock); want_avatar = a->avatar_enabled; bsdr_mutex_unlock(a->lock);
    if (ok) {
        /* The avatar data channel must target the BOT'S OWN mediaPeer (its assigned dataPort), NOT the
         * presenting screen's data producer. The join response's me.mediaPeer is often empty ({}), so its
         * flat dataPort actually belongs to the SCREEN (e.g. 47700) — an SCTP INIT there never associates
         * (mediasoup has that port latched to the owner's producer), leaving the bot a userlist ghost with
         * no avatar. GET /room returns the bot's own me.mediaPeer (dataPort e.g. 41943) plus our
         * legacyUserId (the Quest keys remote avatars by it) and seatIndex — prefer all three from there. */
        bsdr_cloud_screen full; memset(&full, 0, sizeof full);
        int have_full = (room.room_id[0] && bsdr_cloud_get_room(bot_tok, room.room_id, &full) && full.found);

        /* Remember the bot's own userSessionId (marks its entry/SSRC in the roster) and pull the full
         * participant roster so access-control + the audio-volume policy know who is present. */
        if (have_full && full.session_id[0]) {
            bsdr_mutex_lock(a->lock);
            snprintf(a->bot_session_id, sizeof a->bot_session_id, "%s", full.session_id);
            bsdr_mutex_unlock(a->lock);
        }
        if (room.room_id[0]) {
            bsdr_roster tmp;
            bsdr_cloud_get_participants(bot_tok, room.room_id, &tmp, full.session_id);
            bsdr_mutex_lock(a->lock); a->roster = tmp; bsdr_mutex_unlock(a->lock);
        }

        const char *rip = (have_full && full.media_ip[0]) ? full.media_ip
                        : (peer.media_ip[0] ? peer.media_ip : room.media_ip);
        int dport = (have_full && full.data_port > 0) ? full.data_port
                  : (peer.data_port ? peer.data_port : room.data_port);
        char legacy[64] = "";
        snprintf(legacy, sizeof legacy, "%s",
                 (have_full && full.legacy_user_id[0]) ? full.legacy_user_id : peer.legacy_user_id);
        int seat = (have_full && full.seat_index >= 0) ? full.seat_index : peer.seat_index;
        if (seat < 0) seat = 0;                         /* last resort: a valid seat, never -1 */
        /* replace any presence from a previous join */
        bsdr_mutex_lock(a->lock);
        bsdr_botroom *old_room = (bsdr_botroom *)a->bot_room; a->bot_room = NULL;
        bsdr_botmic  *old_mic  = (bsdr_botmic *)a->bot_mic;   a->bot_mic  = NULL;
        bsdr_mutex_unlock(a->lock);
        if (old_room) bsdr_botroom_stop(old_room);
        if (old_mic)  bsdr_botmic_stop(old_mic);
        /* ROOM-MIC producer — the media presence that unlocks the owner mic. Runs for ANY join (audio and
         * fullbot), skipped only by --no-audio. In fullbot mode it also carries the bot's TTS voice; in
         * bare audio mode it stays silent keepalive, which is enough — the Quest only needs to SEE the
         * producer to count the bot as a real co-participant. Uses the bot's OWN mediaPeer micPort +
         * session id from GET /room. Keepalive always on (NULL flag = continuous silence, like a real
         * client). */
        bool no_mic; bsdr_mutex_lock(a->lock); no_mic = !a->audio; bsdr_mutex_unlock(a->lock);
        if (no_mic) {
            BSDR_WARN("bsdr.app", "bot: room-mic producer SKIPPED (--no-audio) — the Quest may not unlock the owner mic");
        } else if (have_full && full.mic_port > 0 && full.session_id[0]) {
            bsdr_botmic *bm = bsdr_botmic_start(rip, full.mic_port, full.session_id, NULL /*keepalive always on*/);
            bsdr_mutex_lock(a->lock); a->bot_mic = bm; bsdr_mutex_unlock(a->lock);
            BSDR_INFO("bsdr.app", "bot: room-mic producer up (%s:%d) — audio presence for the owner-mic unlock",
                      rip ? rip : "(none)", full.mic_port);
        } else {
            BSDR_WARN("bsdr.app", "bot: no mic peer from the cloud (GET /room lacked mic_port/session) — "
                      "the owner-mic unlock may not trigger");
        }

        /* AVATAR — a separate data-channel presence, only when enabled (fullbot / avatar toggle). */
        if (want_avatar && rip && rip[0] && dport > 0 && legacy[0]) {
            BSDR_INFO("bsdr.app", "full-bot: avatar prefix legacyUserId=%s seat=%d relay=%s data=%d", legacy, seat, rip, dport);
            bsdr_botroom *br = bsdr_botroom_start(rip, dport, legacy, seat);
            bsdr_mutex_lock(a->lock); a->bot_room = br; bsdr_mutex_unlock(a->lock);
            if (br) bot_set_msg(a, true, room.room_id, "joined — bringing up avatar\xE2\x80\xA6");
            else    bot_set_msg(a, true, room.room_id, "joined (avatar transport unavailable — audio-only)");
        } else if (want_avatar && rip && rip[0] && dport > 0) {
            BSDR_WARN("bsdr.app", "full-bot: no legacyUserId from the cloud (join + GET /room both lacked it) "
                      "— the avatar can't render; staying joined audio-only. Capture the room JSON to confirm the key.");
            bot_set_msg(a, true, room.room_id, "joined (no legacyUserId for the avatar — check the log)");
        } else if (want_avatar) {
            BSDR_WARN("bsdr.app", "full-bot: join returned no data peer (relay=%s data=%d) — avatar skipped",
                      rip ? rip : "(none)", dport);
            bot_set_msg(a, true, room.room_id, "joined (no data peer for the avatar — check the log)");
        } else {
            bot_set_msg(a, true, room.room_id, "joined the host's room (audio presence up)");
        }
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

/* Registered-plugin-mode lookup (caller holds the lock). -1 if not a plugin mode. */
static int bot_mode_find(bsdr_app *a, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < a->bot_modes_n; i++)
        if (strcmp(a->bot_modes[i].name, name) == 0) return i;
    return -1;
}

void bsdr_app_bot_mode_register(bsdr_app *a, const char *name, bsdr_bot_mode_cb cb, void *user) {
    if (!a || !name || !name[0] || strlen(name) >= sizeof a->bot_modes[0].name) return;
    if (strcmp(name, "audio") == 0) return;                 /* "audio" is the reserved core built-in */
    bsdr_bot_mode_cb activate = NULL; void *au = NULL;
    bsdr_mutex_lock(a->lock);
    int i = bot_mode_find(a, name);
    if (i < 0 && a->bot_modes_n < (int)(sizeof a->bot_modes / sizeof a->bot_modes[0])) i = a->bot_modes_n++;
    if (i >= 0) {
        snprintf(a->bot_modes[i].name, sizeof a->bot_modes[i].name, "%s", name);
        a->bot_modes[i].cb = cb; a->bot_modes[i].user = user;
        if (strcmp(a->bot_mode, name) == 0) { activate = cb; au = user; }   /* already-selected (persisted) */
    }
    bsdr_mutex_unlock(a->lock);
    if (activate) activate(1, au);                          /* activate outside the lock */
    BSDR_INFO("bsdr.app", "bot mode '%s' registered%s", name, activate ? " (active)" : "");
}

/* Lazily create the shared injector for the plugin input services. */
static bsdr_injector *app_input_inj(bsdr_app *a) {
    if (!a->input_inj) a->input_inj = bsdr_injector_create(1920, 1080);
    return (bsdr_injector *)a->input_inj;
}
void bsdr_app_input_type(bsdr_app *a, const char *text)  { if (a) bsdr_cc_type(app_input_inj(a), text); }
void bsdr_app_input_key(bsdr_app *a, const char *keys)   { if (a) bsdr_cc_key(app_input_inj(a), keys); }
void bsdr_app_input_click(bsdr_app *a, double x, double y, const char *button) { if (a) bsdr_cc_click(app_input_inj(a), x, y, button); }
void bsdr_app_input_scroll(bsdr_app *a, int amount)      { if (a) bsdr_cc_scroll(app_input_inj(a), amount); }

/* True once any plugin has registered a bot presence mode — i.e. a plugin owns "the bot". The core uses
 * this to stay BARE: it won't spin up its own in-core command router or apply its ACL audio gains, so
 * "audio" mode is truly just join + audio and the plugin is the sole brain. With no such plugin the core
 * keeps its legacy in-core behaviour. */
int bsdr_app_has_plugin_bot(bsdr_app *a) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock);
    int has = a->bot_modes_n > 0;
    bsdr_mutex_unlock(a->lock);
    return has;
}

void bsdr_app_bot_mode_get(bsdr_app *a, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    snprintf(out, cap, "%s", a->bot_mode[0] ? a->bot_mode : "audio");
    bsdr_mutex_unlock(a->lock);
}

size_t bsdr_app_bot_modes_json(bsdr_app *a, char *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t o = (size_t)snprintf(out, cap, "[\"audio\"");
    if (a) {
        bsdr_mutex_lock(a->lock);
        for (int i = 0; i < a->bot_modes_n && o < cap - 40; i++) {
            char ne[32]; bsdr_json_escape(ne, sizeof ne, a->bot_modes[i].name);
            o += (size_t)snprintf(out + o, cap - o, ",\"%s\"", ne);
        }
        bsdr_mutex_unlock(a->lock);
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

void bsdr_app_bot_set_mode(bsdr_app *a, const char *mode) {
    if (!a || !mode) return;
    /* Validate: "audio" (built-in) or a registered plugin mode; anything else coerces to "audio". Switch
     * fires the OLD plugin mode's cb(0) then the NEW plugin mode's cb(1), so a plugin brings its
     * behaviour (hearing + avatar) up/down itself. */
    bsdr_bot_mode_cb old_cb = NULL, new_cb = NULL; void *old_u = NULL, *new_u = NULL;
    char newname[16];
    bsdr_mutex_lock(a->lock);
    int isaudio = (strcmp(mode, "audio") == 0);
    int mi = isaudio ? -1 : bot_mode_find(a, mode);
    if (!isaudio && mi < 0) { mode = "audio"; isaudio = 1; }
    snprintf(newname, sizeof newname, "%s", mode);
    if (strcmp(a->bot_mode, newname) != 0) {
        int oi = bot_mode_find(a, a->bot_mode);
        if (oi >= 0) { old_cb = a->bot_modes[oi].cb; old_u = a->bot_modes[oi].user; }
        snprintf(a->bot_mode, sizeof a->bot_mode, "%s", newname);
        if (mi >= 0) { new_cb = a->bot_modes[mi].cb; new_u = a->bot_modes[mi].user; }
    }
    bsdr_mutex_unlock(a->lock);
    if (old_cb) old_cb(0, old_u);
    if (new_cb) new_cb(1, new_u);
    settings_save(a);
    BSDR_INFO("bsdr.app", "bot mode -> %s", newname);
}

void bsdr_app_bot_mode_clear_plugin_modes(bsdr_app *a) {
    if (!a) return;
    bsdr_bot_mode_cb active_cb = NULL; void *active_u = NULL; int reset = 0;
    bsdr_mutex_lock(a->lock);
    int oi = bot_mode_find(a, a->bot_mode);
    if (oi >= 0) { active_cb = a->bot_modes[oi].cb; active_u = a->bot_modes[oi].user; reset = 1; }
    a->bot_modes_n = 0;
    if (reset) snprintf(a->bot_mode, sizeof a->bot_mode, "audio");
    bsdr_mutex_unlock(a->lock);
    if (active_cb) active_cb(0, active_u);        /* deactivate the active plugin mode before it unmaps */
    if (reset) bsdr_app_set_avatar_enabled(a, 0); /* and make sure the avatar is down */
}

void bsdr_app_set_gain_policy(bsdr_app *a, bsdr_gain_policy_fn fn, void *user) {
    if (!a) return;
    bsdr_mutex_lock(a->lock);
    a->gain_policy_fn = fn; a->gain_policy_user = user;
    bsdr_mutex_unlock(a->lock);
    BSDR_INFO("bsdr.app", "per-speaker gain policy %s", fn ? "taken over by a plugin" : "back to core default");
}

/* botaudio calls this each policy cycle: a plugin policy if one is registered, else the core ACL policy.
 * Returns the pair count (or -1 for "no policy — leave unity"). */
int bsdr_app_apply_gain_policy(bsdr_app *a, uint32_t *ssrc_out, float *gain_out, int cap, float *default_out) {
    if (!a) return -1;
    bsdr_mutex_lock(a->lock);
    bsdr_gain_policy_fn fn = a->gain_policy_fn; void *u = a->gain_policy_user;
    bsdr_mutex_unlock(a->lock);
    if (fn) return fn(u, ssrc_out, gain_out, cap, default_out);   /* plugin owns the policy */
    /* No plugin policy: if a plugin owns the bot, stay BARE (unity — silence no one); only the legacy
     * no-plugin core applies its ACL-based per-speaker gains. */
    if (bsdr_app_has_plugin_bot(a)) return -1;
    return bsdr_app_audio_gains(a, ssrc_out, gain_out, cap, default_out);
}

void bsdr_app_set_avatar_enabled(bsdr_app *a, int on) {
    if (!a) return;
    bsdr_botroom *stop = NULL;
    bsdr_mutex_lock(a->lock);
    a->avatar_enabled = on ? true : false;
    if (!on) { stop = (bsdr_botroom *)a->bot_room; a->bot_room = NULL; }
    bsdr_mutex_unlock(a->lock);
    if (stop) bsdr_botroom_stop(stop);
    BSDR_INFO("bsdr.app", "avatar actuator -> %s%s", on ? "on" : "off",
              on ? " (renders on the next bot (re)join)" : "");
}

int bsdr_app_get_bot_follow(bsdr_app *a) {
    if (!a) return 0;
    bsdr_mutex_lock(a->lock); int v = a->bot_follow ? 1 : 0; bsdr_mutex_unlock(a->lock);
    return v;
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
    /* Debounce: pulling a JOINED bot out of its room (operator left, or moved elsewhere) requires
     * several CONSECUTIVE confirming polls. A single /rooms hiccup — the headset briefly asleep, a
     * transient 5xx, or the operator's room momentarily not listed first — must not evict the bot,
     * which is why an idle audio-only bot used to "join then leave" on its own. Joining an unjoined
     * bot is NOT debounced (follow should attach promptly when you enable it). */
    static int miss = 0;
    #define FOLLOW_CONFIRM 3            /* ~3 polls (~45-60s) of agreement before we act */
    if (!a) return;
    char host_tok[2048], cur[128];
    bsdr_mutex_lock(a->lock);
    int active = a->bot_follow && a->cloud_logged_in && a->bot_logged_in && !a->bot_stopped;
    snprintf(host_tok, sizeof host_tok, "%s", a->access_token);
    snprintf(cur, sizeof cur, "%s", a->bot_room_id);   /* room the bot is currently in */
    bsdr_mutex_unlock(a->lock);
    if (!active || !host_tok[0]) { miss = 0; return; }

    char op_room[128] = "";
    if (!bsdr_cloud_poll_room_id(host_tok, op_room, sizeof op_room)) {
        /* Operator not reported in any room. Only pull a joined bot out after repeated confirmations. */
        if (cur[0] && a->bot_joined) {
            if (++miss < FOLLOW_CONFIRM) {
                BSDR_DEBUG("bsdr.app", "follow-me: operator room not seen (%d/%d) — holding %s",
                           miss, FOLLOW_CONFIRM, cur);
                return;
            }
            miss = 0;
            BSDR_INFO("bsdr.app", "follow-me: operator left (confirmed); the bot leaves %s", cur);
            bsdr_app_bot_leave_room(a);
        } else miss = 0;
        return;
    }
    /* Same room (bare-vs-prefixed tolerant): nothing to do. */
    const char *c = cur, *o = op_room;
    if (strncmp(c, "room:", 5) == 0) c += 5;
    if (strncmp(o, "room:", 5) == 0) o += 5;
    if (a->bot_joined && strcmp(c, o) == 0) { miss = 0; return; }

    /* Different room. If the bot is already joined somewhere, this is a MOVE — confirm it repeatedly
     * before tearing the good session down (guards against a spurious first-roomId in /rooms). A bot
     * that isn't joined yet attaches immediately. */
    if (a->bot_joined && cur[0]) {
        if (++miss < FOLLOW_CONFIRM) {
            BSDR_DEBUG("bsdr.app", "follow-me: operator seen in %s vs bot %s (%d/%d) — holding",
                       op_room, cur, miss, FOLLOW_CONFIRM);
            return;
        }
    }
    miss = 0;
    BSDR_INFO("bsdr.app", "follow-me: operator in %s (bot was in %s) — following", op_room,
              cur[0] ? cur : "(none)");
    if (cur[0] && a->bot_joined) bsdr_app_bot_leave_room(a);   /* drop the old room first */
    bsdr_app_bot_join_room(a);                                 /* re-resolves + joins the operator's room */
    #undef FOLLOW_CONFIRM
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

/* Same, for the BOT account (its own client-key session + refresh token). The bot's short-lived access
 * token expires mid-session just like the host's; without this every bot call (join, /social/profile)
 * 403s with "requires a valid entry for 'x-access-token'". Updates + persists the new bot token. */
static bool bot_renew_token(bsdr_app *a) {
    char refresh[2048];
    bsdr_mutex_lock(a->lock);
    snprintf(refresh, sizeof(refresh), "%s", a->bot_refresh_token);
    bsdr_mutex_unlock(a->lock);
    if (!refresh[0]) return false;
    bsdr_cloud_result rr;
    if (!bsdr_cloud_renew(bsdr_cloud_client_key(), refresh, &rr)) { BSDR_WARN("bsdr.app", "bot token renew failed"); return false; }
    bsdr_mutex_lock(a->lock);
    snprintf(a->bot_access_token, sizeof(a->bot_access_token), "%s", rr.access_token);
    if (rr.refresh_token[0]) snprintf(a->bot_refresh_token, sizeof(a->bot_refresh_token), "%s", rr.refresh_token);
    a->bot_token_ms = bsdr_now_ms();
    bsdr_mutex_unlock(a->lock);
    bot_session_save(a);
    BSDR_INFO("bsdr.app", "bot token renewed");
    return true;
}

/* Proactive bot-token renewal: renew ~10 min after the token was issued (they last ~15 min), so the
 * bot's calls (join, /social/profile, roster) never hit an expired-token 403. Cheap — the elapsed-time
 * guard makes this a no-op on all but one call per ~10 min. Driven from the agent's periodic tick. */
void bsdr_app_bot_token_tick(bsdr_app *a) {
    if (!a) return;
    uint64_t issued; int go;
    bsdr_mutex_lock(a->lock);
    go = a->bot_logged_in && !a->bot_stopped && a->bot_refresh_token[0];
    issued = a->bot_token_ms;
    bsdr_mutex_unlock(a->lock);
    if (!go) return;
    uint64_t now = bsdr_now_ms();
    if (issued != 0 && now - issued < 10u * 60u * 1000u) return;   /* still fresh */
    bot_renew_token(a);   /* refreshes bot_token_ms on success */
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

void bsdr_app_set_file_loop(bsdr_app *a, int on) {
    bsdr_mutex_lock(a->lock);
    a->file_loop = on ? 1 : 0;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);   /* persist the loop preference */
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

void bsdr_app_set_terminal(bsdr_app *a, const char *backend, int cols, int rows) {
    bsdr_mutex_lock(a->lock);
    if (backend && backend[0])
        snprintf(a->term_backend, sizeof(a->term_backend), "%s", strcmp(backend, "xvfb") == 0 ? "xvfb" : "pty");
    if (cols > 0) a->term_cols = cols < 20 ? 20 : cols > 400 ? 400 : cols;
    if (rows > 0) a->term_rows = rows < 6 ? 6 : rows > 120 ? 120 : rows;
    bsdr_mutex_unlock(a->lock);
    settings_save(a);
}

void bsdr_app_get_terminal(bsdr_app *a, char *backend, size_t bl, int *cols, int *rows) {
    bsdr_mutex_lock(a->lock);
    if (backend) snprintf(backend, bl, "%s", a->term_backend[0] ? a->term_backend : "pty");
    if (cols) *cols = a->term_cols > 0 ? a->term_cols : 120;
    if (rows) *rows = a->term_rows > 0 ? a->term_rows : 36;
    bsdr_mutex_unlock(a->lock);
}

size_t bsdr_app_status_json(bsdr_app *a, char *out, size_t cap) {
    bsdr_mutex_lock(a->lock);
    char esc_name[160], esc_email[160], esc_msg[200], esc_sniff[200], esc_cc[200], esc_ai[400], esc_cdp[300];
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
    bsdr_json_escape(esc_cdp, sizeof(esc_cdp), a->cdp_endpoint);
    bsdr_json_escape(esc_ai, sizeof(esc_ai), a->threed_ai_cmd);
    if (a->sniff_wifi < 0)   /* resolve once, then cache (default iface rarely changes) */
        a->sniff_wifi = bsdr_micsniff_default_wireless() ? 1 : 0;
    /* Live avatar-plane state (full bot): connecting/up/ghost while a presence thread runs, else off.
     * Lets the UI show real progress after a join instead of a premature "avatar up". */
    const char *avatar_st = "off";
    switch (bsdr_botroom_avatar_state((bsdr_botroom *)a->bot_room)) {
        case BSDR_AVATAR_CONNECTING: avatar_st = "connecting"; break;
        case BSDR_AVATAR_UP:         avatar_st = "up";         break;
        case BSDR_AVATAR_GHOST:      avatar_st = "ghost";      break;
        default:                     avatar_st = "off";        break;
    }
    char e_tpiper[300], e_tmodel[600], e_tendp[300], e_tcm[96], e_tvoice[96];
    bsdr_json_escape(e_tpiper, sizeof e_tpiper, a->tts.piper);
    bsdr_json_escape(e_tmodel, sizeof e_tmodel, a->tts.model);
    bsdr_json_escape(e_tendp,  sizeof e_tendp,  a->tts.endpoint);
    bsdr_json_escape(e_tcm,    sizeof e_tcm,    a->tts.cloud_model);
    bsdr_json_escape(e_tvoice, sizeof e_tvoice, a->tts.voice);
    /* Selectable bot presence modes: built-in "audio" + any a plugin registered. Built lock-free here
     * (we already hold a->lock) to avoid re-locking via bsdr_app_bot_modes_json. */
    char botmodes[160]; size_t bmo = (size_t)snprintf(botmodes, sizeof botmodes, "[\"audio\"");
    for (int i = 0; i < a->bot_modes_n && bmo < sizeof botmodes - 40; i++) {
        char mne[24]; bsdr_json_escape(mne, sizeof mne, a->bot_modes[i].name);
        bmo += (size_t)snprintf(botmodes + bmo, sizeof botmodes - bmo, ",\"%s\"", mne);
    }
    snprintf(botmodes + bmo, sizeof botmodes - bmo, "]");
    int n = snprintf(out, cap,
        "{\"cloud\":{\"loggedIn\":%s,\"email\":\"%s\",\"name\":\"%s\",\"msg\":\"%s\","
        "\"internetSharing\":%s},"
        "\"bot\":{\"loggedIn\":%s,\"email\":\"%s\",\"name\":\"%s\",\"msg\":\"%s\",\"joined\":%s,\"room\":\"%s\",\"mode\":\"%s\",\"modes\":%s,\"pluginBot\":%s,\"stopped\":%s,\"follow\":%s,\"loopback\":%s,\"solo\":%s,\"avatar\":\"%s\"},"
        "\"quest\":{\"paired\":%s,\"name\":\"%s\",\"ip\":\"%s\",\"streaming\":%s,\"paused\":%s},"
        "\"source\":{\"mode\":\"%s\",\"path\":\"%s\",\"path2\":\"%s\",\"audio\":%s,\"fileLoop\":%s,"
        "\"termBackend\":\"%s\",\"termCols\":%d,\"termRows\":%d},"
        "\"blank\":%s,\"pointerTouch\":%s,\"cloudMic\":%s,\"ownerMicLocal\":%s,\"ownerMicToQuestMic\":%s,\"roomMic\":%s,\"tlsInsecure\":%s,"
        "\"threed\":{\"mode\":%d,\"deepness\":%d,\"convergence\":%d,\"swap\":%s,\"full\":%s,\"tier\":%d,\"ai\":\"%s\"},"
        "\"quality\":{\"w\":%d,\"h\":%d,\"bitrate\":%d,\"brOverride\":%d,\"gpuEncode\":%s,\"encLevel\":%d,\"x264Threads\":%d,\"vaapi\":%s,\"kmsgrab\":%s,\"lan1x\":%s,\"fpsCap\":%d,\"wifiOpt\":%s,\"cloudPli\":%s},"
        "\"voice\":{\"stt\":\"%s\",\"sttModel\":\"%s\",\"sttToken\":%s,"
        "\"llm\":\"%s\",\"llmModel\":\"%s\",\"llmToken\":%s},"
        "\"tts\":{\"enabled\":%s,\"engine\":%d,\"route\":%d,\"piper\":\"%s\",\"model\":\"%s\","
        "\"endpoint\":\"%s\",\"cloudModel\":\"%s\",\"voice\":\"%s\",\"tokenSet\":%s},"
        "\"sniff\":{\"want\":%s,\"mitm\":%s,\"method\":%d,\"active\":%s,\"msg\":\"%s\",\"wifi\":%s,"
        "\"fxOn\":%s,\"gender\":%d,\"formant\":%d,\"volume\":%d,\"robot\":%d,\"echo\":%d,\"whisper\":%d,\"substitute\":%s,\"relayPort\":%d},"
        "\"compctl\":{\"want\":%s,\"vision\":%s,\"active\":%s,\"msg\":\"%s\",\"browserCtl\":%s,\"cdp\":\"%s\"},"
        "\"android\":%s,\"selected\":\"%s\",\"quests\":[",
        a->cloud_logged_in ? "true" : "false", esc_email, esc_name, esc_msg,
        a->internet_sharing ? "true" : "false",
        a->bot_logged_in ? "true" : "false", esc_botemail, esc_botname, esc_botmsg,
        a->bot_joined ? "true" : "false", esc_botroom, a->bot_mode[0] ? a->bot_mode : "audio", botmodes,
        a->bot_modes_n > 0 ? "true" : "false",
        a->bot_stopped ? "true" : "false", a->bot_follow ? "true" : "false",
        a->bot_loopback ? "true" : "false", a->bot_solo_owner ? "true" : "false", avatar_st,
        a->quest_paired ? "true" : "false", a->quest_name, a->quest_ip,
        a->streaming ? "true" : "false", a->paused ? "true" : "false",
        a->source, a->source_path, a->source_path2, a->audio ? "true" : "false", a->file_loop ? "true" : "false",
        a->term_backend[0] ? a->term_backend : "pty", a->term_cols, a->term_rows,
        a->blank_want ? "true" : "false", a->pointer_touch ? "true" : "false", a->cloud_mic_fallback ? "true" : "false",
        a->owner_mic_local ? "true" : "false", a->owner_mic_to_questmic ? "true" : "false", a->room_mic_want ? "true" : "false",
        bsdr_tls_is_insecure() ? "true" : "false",
        a->threed_mode, a->threed_deepness, a->threed_convergence,
        a->threed_swap ? "true" : "false", a->threed_full ? "true" : "false", a->threed_tier, esc_ai,
        a->res_w, a->res_h, a->bitrate, a->bitrate_override, a->cpu_only ? "false" : "true",
        a->enc_level, a->enc_x264_threads, a->use_vaapi ? "true" : "false", a->use_kmsgrab ? "true" : "false",
        a->lan_1x ? "true" : "false", a->fps_cap,
        a->wifi_opt ? "true" : "false", a->cloud_rtcp_pli ? "true" : "false",
        a->stt_endpoint, a->stt_model, a->stt_token[0] ? "true" : "false",
        a->llm_endpoint, a->llm_model, a->llm_token[0] ? "true" : "false",
        a->tts_enabled ? "true" : "false", (int)a->tts.engine, a->tts_route,
        e_tpiper, e_tmodel, e_tendp, e_tcm, e_tvoice, a->tts.token[0] ? "true" : "false",
        a->sniff_want ? "true" : "false", a->sniff_mitm ? "true" : "false", a->sniff_method,
        a->sniff_active ? "true" : "false", esc_sniff, a->sniff_wifi > 0 ? "true" : "false",
        a->voice_fx_on ? "true" : "false",
        a->voice_gender, a->voice_formant, a->voice_volume, a->voice_robot, a->voice_echo, a->voice_whisper,
        a->voice_substitute ? "true" : "false", a->sniff_remote_port,
        a->compctl_want ? "true" : "false", a->compctl_vision ? "true" : "false",
        a->compctl_active ? "true" : "false", esc_cc,
        a->browser_ctl_enabled ? "true" : "false", esc_cdp,
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
                      "false",   /* core has no voice-ai engine now; the voice-changer plugin reports its own */
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
    /* Release the app lock BEFORE calling into any plugin hook below: a plugin's ui/panel/sections hook
     * may call back into an app host service (e.g. fullbot's panel reads bsdr_app_avatar_state), which
     * re-locks a->lock — a self-deadlock if we still held it. Everything below only appends to `out`
     * using local `n`/`cap`; no more app state is read. */
    bsdr_mutex_unlock(a->lock);
    /* Loadable plugins contribute their own bot-card UI (name + HTML fragment); the panel renders
     * s.plugins[] into #plugins. Empty array when no plugin exposes a ui_html hook. */
    n += snprintf(out + n, cap - n, ",\"pluginsLoaded\":");
    if (n > 0 && (size_t)n < cap) n += (int)bsdr_plugins_names_json(out + n, cap - (size_t)n);
    n += snprintf(out + n, cap - n, ",\"plugins\":");
    if (n > 0 && (size_t)n < cap) n += (int)bsdr_plugins_ui_json(out + n, cap - (size_t)n);
    /* Plugins may also contribute full top-level panels (own cards) and declared config variables;
     * the panel renders s.pluginPanels[] as cards and s.pluginConfig[] as auto-generated forms. */
    n += snprintf(out + n, cap - n, ",\"pluginPanels\":");
    if (n > 0 && (size_t)n < cap) n += (int)bsdr_plugins_panel_json(out + n, cap - (size_t)n);
    n += snprintf(out + n, cap - n, ",\"pluginSections\":");
    if (n > 0 && (size_t)n < cap) n += (int)bsdr_plugins_sections_json(out + n, cap - (size_t)n);
    n += snprintf(out + n, cap - n, ",\"pluginScripts\":");
    if (n > 0 && (size_t)n < cap) n += (int)bsdr_plugins_scripts_json(out + n, cap - (size_t)n);
    n += snprintf(out + n, cap - n, ",\"pluginConfig\":");
    if (n > 0 && (size_t)n < cap) n += (int)bsdr_plugins_config_json(out + n, cap - (size_t)n);
    n += snprintf(out + n, cap - n, "}");
    return (size_t)n;
}
