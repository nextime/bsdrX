/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Per-speaker utterance router — see bsdr/roomcmd.h. */
#include "bsdr/roomcmd.h"
#include "bsdr/app.h"
#include "bsdr/stt.h"
#include "bsdr/llm.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RC_RATE        48000
#define RC_MAX_SEC     10
#define RC_SILENCE_MS  800     /* end an utterance this long after speech stops */
#define RC_MIN_MS      350     /* ignore blips shorter than this */
#define RC_SLOTS       8       /* concurrent speakers we track VAD for */
#define RC_QUEUE_MAX   6

/* Per-speaker capture + VAD state. buf is interleaved int16; allocated on first use. */
typedef struct {
    uint32_t ssrc;
    int      channels;
    int16_t *buf;
    size_t   count;      /* samples buffered (frames*channels) */
    size_t   cap;        /* buf capacity in samples */
    int      speaking;
    int      silence_ms;
    float    noise;      /* adaptive noise floor (mean-abs) */
    int      active;
} rc_slot;

/* A completed utterance queued to the worker. */
typedef struct rc_job {
    uint32_t ssrc;
    int      channels;
    int16_t *pcm;        /* owned */
    int      frames;     /* samples per channel */
    struct rc_job *next;
} rc_job;

struct bsdr_roomcmd {
    bsdr_app        *app;
    bsdr_mutex      *lock;      /* guards slots + queue */
    bsdr_cond       *cv;
    bsdr_thread     *worker;
    volatile int     stop;
    volatile int     enabled;   /* processing on/off (tap drops audio when off) */
    rc_slot          slots[RC_SLOTS];
    rc_job          *q_head, *q_tail;
    int              q_len;
    /* One-shot translation arm (set by the translate_next tool). Guarded by lock. */
    uint32_t         xlate_ssrc;
    char             xlate_lang[32];
    int              xlate_active;
    /* Mic-check arm (age verification). Guarded by lock. */
    uint32_t         mic_ssrc;
    char             mic_user[64];
    int              mic_active;
    uint32_t         checked[32];   /* SSRCs already auto-checked this session (dedup) */
    int              nchecked;
    volatile uint32_t owner_ssrc;   /* pushed by botaudio; used for overload protection (lock-free) */
    int              overloaded;    /* backlog > threshold: only the owner is served (log-once) */
};

#define RC_OVERLOAD 3   /* > this many queued utterances => serve only the owner until it drains */

/* mean absolute amplitude of an interleaved chunk (cheap voice-activity level, no sqrt). */
static float mean_abs(const int16_t *p, size_t n) {
    if (!n) return 0;
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i] < 0 ? -p[i] : p[i];
    return (float)s / (float)n;
}

static void worker_fn(void *arg);

bsdr_roomcmd *bsdr_roomcmd_new(bsdr_app *app) {
    bsdr_roomcmd *rc = calloc(1, sizeof *rc);
    if (!rc) return NULL;
    rc->app = app;
    rc->lock = bsdr_mutex_new();
    rc->cv = bsdr_cond_new();
    rc->worker = bsdr_thread_start(worker_fn, rc);   /* idle until an utterance is queued */
    return rc;
}

static rc_slot *slot_for(bsdr_roomcmd *rc, uint32_t ssrc, int channels) {
    rc_slot *lru = &rc->slots[0];
    for (int i = 0; i < RC_SLOTS; i++) {
        if (rc->slots[i].active && rc->slots[i].ssrc == ssrc) return &rc->slots[i];
        if (!rc->slots[i].active) lru = &rc->slots[i];
    }
    /* reuse an inactive (or the first) slot */
    if (lru->buf) lru->count = 0;
    lru->ssrc = ssrc; lru->channels = channels; lru->count = 0;
    lru->speaking = 0; lru->silence_ms = 0; lru->noise = 200.0f; lru->active = 1;
    if (!lru->buf) {
        lru->cap = (size_t)RC_RATE * RC_MAX_SEC * (channels > 0 ? channels : 1);
        lru->buf = malloc(lru->cap * sizeof(int16_t));
        if (!lru->buf) { lru->active = 0; return NULL; }
    }
    return lru;
}

/* Enqueue a finished utterance (copy of the slot's buffer). Caller holds rc->lock. */
static void enqueue(bsdr_roomcmd *rc, rc_slot *s) {
    int frames = (int)(s->count / (s->channels > 0 ? s->channels : 1));
    if (frames < RC_RATE * RC_MIN_MS / 1000) { s->count = 0; s->speaking = 0; s->silence_ms = 0; return; }
    /* Overload protection: once the backlog exceeds the threshold, serve ONLY the owner and drop
     * everyone else until it drains — so a flood of simultaneous speakers can't swamp the bot. */
    int othr = (rc->app && rc->app->overload_threshold >= 1) ? rc->app->overload_threshold : RC_OVERLOAD;
    if (rc->q_len >= othr && s->ssrc != rc->owner_ssrc) {
        if (!rc->overloaded) { rc->overloaded = 1; BSDR_INFO("bsdr.roomcmd", "overloaded (%d queued) — owner-only until it clears", rc->q_len); }
        s->count = 0; s->speaking = 0; s->silence_ms = 0; return;
    }
    if (rc->q_len >= RC_QUEUE_MAX) { s->count = 0; s->speaking = 0; s->silence_ms = 0; return; }
    rc_job *j = calloc(1, sizeof *j);
    if (!j) { s->count = 0; s->speaking = 0; s->silence_ms = 0; return; }
    j->pcm = malloc(s->count * sizeof(int16_t));
    if (!j->pcm) { free(j); s->count = 0; s->speaking = 0; s->silence_ms = 0; return; }
    memcpy(j->pcm, s->buf, s->count * sizeof(int16_t));
    j->ssrc = s->ssrc; j->channels = s->channels; j->frames = frames;
    if (rc->q_tail) rc->q_tail->next = j; else rc->q_head = j;
    rc->q_tail = j; rc->q_len++;
    bsdr_cond_signal(rc->cv);
    s->count = 0; s->speaking = 0; s->silence_ms = 0;   /* reset for the next utterance */
}

void bsdr_roomcmd_set_enabled(bsdr_roomcmd *rc, int on) { if (rc) rc->enabled = on ? 1 : 0; }
void bsdr_roomcmd_set_owner_ssrc(bsdr_roomcmd *rc, uint32_t ssrc) { if (rc) rc->owner_ssrc = ssrc; }

void bsdr_roomcmd_state(bsdr_roomcmd *rc, bsdr_roomcmd_status *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    if (!rc) return;
    st->enabled   = rc->enabled;
    st->queued    = rc->q_len;               /* volatile-ish read; a status snapshot needn't lock */
    st->overloaded = rc->overloaded;
    bsdr_mutex_lock(rc->lock);
    st->mic_check_active = rc->mic_active;
    snprintf(st->mic_check_user, sizeof st->mic_check_user, "%s", rc->mic_user);
    st->translate_active = rc->xlate_active;
    snprintf(st->translate_lang, sizeof st->translate_lang, "%s", rc->xlate_lang);
    bsdr_mutex_unlock(rc->lock);
}

void bsdr_roomcmd_arm_translate(bsdr_roomcmd *rc, uint32_t target_ssrc, const char *to_language) {
    if (!rc || !target_ssrc) return;
    bsdr_mutex_lock(rc->lock);
    rc->xlate_ssrc = target_ssrc;
    snprintf(rc->xlate_lang, sizeof rc->xlate_lang, "%s", to_language ? to_language : "English");
    rc->xlate_active = 1;
    bsdr_mutex_unlock(rc->lock);
    BSDR_INFO("bsdr.roomcmd", "translation armed: next utterance from ssrc %u -> %s",
              target_ssrc, rc->xlate_lang);
}

/* Mic-check no-response timeout: if the target hasn't answered ~20 s after arming, treat it as a
 * failed check — kick + ban and clear the boost. Runs on a detached thread per mic-check. */
struct mic_timeout { bsdr_roomcmd *rc; uint32_t ssrc; char user[64]; };
static void miccheck_timeout(void *arg) {
    struct mic_timeout *t = (struct mic_timeout *)arg;
    bsdr_sleep_ms(20000);
    int still = 0;
    bsdr_mutex_lock(t->rc->lock);
    if (t->rc->mic_active && t->rc->mic_ssrc == t->ssrc) { t->rc->mic_active = 0; t->rc->mic_ssrc = 0; still = 1; }
    bsdr_mutex_unlock(t->rc->lock);
    if (still && !t->rc->stop) {
        BSDR_INFO("bsdr.roomcmd", "mic-check: %s did not respond in 20s -> ban", t->user);
        bsdr_app_tts_say(t->rc->app, "No response to the age check. Removing them.");
        bsdr_app_ban_user(t->rc->app, t->user);
    }
    bsdr_mutex_lock(t->rc->app->lock); t->rc->app->mic_check_ssrc = 0; bsdr_mutex_unlock(t->rc->app->lock);
    free(t);
}

void bsdr_roomcmd_arm_miccheck(bsdr_roomcmd *rc, uint32_t target_ssrc, const char *username) {
    if (!rc || !target_ssrc) return;
    bsdr_mutex_lock(rc->lock);
    rc->mic_ssrc = target_ssrc;
    snprintf(rc->mic_user, sizeof rc->mic_user, "%s", username ? username : "");
    rc->mic_active = 1;
    bsdr_mutex_unlock(rc->lock);
    bsdr_app_tts_say(rc->app, "Please say a full sentence so I can verify your age.");
    BSDR_INFO("bsdr.roomcmd", "mic-check armed for %s (ssrc %u)", username ? username : "?", target_ssrc);
    struct mic_timeout *t = calloc(1, sizeof *t);
    if (t) { t->rc = rc; t->ssrc = target_ssrc; snprintf(t->user, sizeof t->user, "%s", username ? username : "");
             bsdr_thread_start_detached(miccheck_timeout, t); }
}

int bsdr_roomcmd_autocheck(bsdr_roomcmd *rc, uint32_t target_ssrc, const char *username) {
    if (!rc || !target_ssrc || !rc->enabled) return 0;
    bsdr_mutex_lock(rc->lock);
    /* Skip if a check/translation is already running, or we've auto-checked this SSRC this session. */
    int skip = rc->mic_active || rc->xlate_active;
    for (int i = 0; i < rc->nchecked && !skip; i++) if (rc->checked[i] == target_ssrc) skip = 1;
    if (!skip && rc->nchecked < (int)(sizeof rc->checked / sizeof rc->checked[0]))
        rc->checked[rc->nchecked++] = target_ssrc;
    bsdr_mutex_unlock(rc->lock);
    if (skip) return 0;
    /* Boost the target so they're audible + tapped for the check, then arm it. */
    bsdr_mutex_lock(rc->app->lock); rc->app->mic_check_ssrc = target_ssrc; bsdr_mutex_unlock(rc->app->lock);
    bsdr_roomcmd_arm_miccheck(rc, target_ssrc, username);
    return 1;
}

void bsdr_roomcmd_tap(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user) {
    bsdr_roomcmd *rc = (bsdr_roomcmd *)user;
    if (!rc || rc->stop || !rc->enabled || !pcm || frames <= 0) return;
    size_t n = (size_t)frames * (channels > 0 ? channels : 1);
    int ms = frames * 1000 / RC_RATE;
    float lvl = mean_abs(pcm, n);
    bsdr_mutex_lock(rc->lock);
    rc_slot *s = slot_for(rc, ssrc, channels);
    if (!s) { bsdr_mutex_unlock(rc->lock); return; }
    if (!s->speaking) s->noise = s->noise * 0.95f + lvl * 0.05f;   /* adapt only on silence */
    float thresh = s->noise * 3.0f + 300.0f;
    int voiced = lvl > thresh;
    if (voiced) { s->speaking = 1; s->silence_ms = 0; }
    else if (s->speaking) s->silence_ms += ms;
    if (s->speaking && s->count + n <= s->cap) { memcpy(s->buf + s->count, pcm, n * sizeof(int16_t)); s->count += n; }
    if (s->speaking && (s->silence_ms >= RC_SILENCE_MS || s->count + n > s->cap)) enqueue(rc, s);
    bsdr_mutex_unlock(rc->lock);
}

static void process_job(bsdr_roomcmd *rc, rc_job *j) {
    bsdr_app *app = rc->app;
    /* Snapshot config + identity under the app lock. */
    bsdr_stt_config stt; bsdr_llm_config llm;
    memset(&stt, 0, sizeof stt); memset(&llm, 0, sizeof llm);
    bsdr_mutex_lock(app->lock);
    snprintf(stt.endpoint, sizeof stt.endpoint, "%s", app->stt_endpoint);
    snprintf(stt.token,    sizeof stt.token,    "%s", app->stt_token);
    snprintf(stt.model,    sizeof stt.model,    "%s", app->stt_model);
    snprintf(llm.endpoint, sizeof llm.endpoint, "%s", app->llm_endpoint);
    snprintf(llm.token,    sizeof llm.token,    "%s", app->llm_token);
    snprintf(llm.model,    sizeof llm.model,    "%s", app->llm_model);
    llm.context_tokens   = app->llm_context_tokens > 0 ? app->llm_context_tokens : app->llm_context_detected;
    llm.compact_pct      = app->llm_compact_pct;
    llm.compact_strategy = app->llm_compact_strategy;
    llm.max_rounds       = app->llm_max_rounds;
    bsdr_mutex_unlock(app->lock);
    if (!stt.endpoint[0] || !llm.endpoint[0]) return;   /* not configured */

    /* Only armed speakers are acted on: a one-shot translation, or a mic-check. Nothing else in the
     * room is a command — addressing the bot by name is the fullbot plugin's job, and it owns the
     * hearing outright when loaded (this tap never runs). */
    char xlang[32] = "", micuser[64] = ""; int xlate = 0, miccheck = 0;
    bsdr_mutex_lock(rc->lock);
    if (rc->xlate_active && rc->xlate_ssrc == j->ssrc) {
        xlate = 1; snprintf(xlang, sizeof xlang, "%s", rc->xlate_lang);
        rc->xlate_active = 0; rc->xlate_ssrc = 0;   /* one-shot */
    } else if (rc->mic_active && rc->mic_ssrc == j->ssrc) {
        miccheck = 1; snprintf(micuser, sizeof micuser, "%s", rc->mic_user);
        rc->mic_active = 0; rc->mic_ssrc = 0;        /* one-shot (the timeout thread is now a no-op) */
    }
    bsdr_mutex_unlock(rc->lock);

    if (!xlate && !miccheck) return;

    /* Transcribe. */
    char text[2048] = "";
    if (!bsdr_stt_transcribe(&stt, j->pcm, j->frames, RC_RATE, j->channels, text, sizeof text) || !text[0]) {
        if (xlate || miccheck) { bsdr_mutex_lock(app->lock); app->mic_check_ssrc = 0; bsdr_mutex_unlock(app->lock); }
        return;
    }

    /* --- mic-check path: judge whether the speaker is an adult; kick+ban if not --- */
    if (miccheck) {
        bsdr_mutex_lock(app->lock); app->mic_check_ssrc = 0; bsdr_mutex_unlock(app->lock);  /* release boost */
        char verdict[64] = "";
        bsdr_llm_run_ex(&llm,
            "You verify whether a speaker is an adult (18+) from what they just said. Judge from "
            "vocabulary, content and coherence. Reply with exactly one word: ADULT, MINOR, or UNSURE.",
            text, NULL, NULL, NULL, NULL, 0, NULL, verdict, sizeof verdict);
        BSDR_INFO("bsdr.roomcmd", "mic-check %s -> %s (heard: %.60s)", micuser, verdict, text);
        for (char *p = verdict; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (strstr(verdict, "ADULT")) {   /* anything not a clear adult => fail (kick + ban) */
            bsdr_app_tts_say(app, "Thanks, you're verified. Welcome.");
        } else {
            bsdr_app_tts_say(app, "Sorry, I can't verify you're an adult. Removing you from the room.");
            bsdr_app_ban_user(app, micuser);
        }
        return;
    }

    /* --- translation path: translate this utterance into the target language and speak it --- */
    if (xlate) {
        bsdr_mutex_lock(app->lock); app->mic_check_ssrc = 0; bsdr_mutex_unlock(app->lock);  /* release the boost */
        char sysx[256];
        snprintf(sysx, sizeof sysx,
            "You are a translator. Translate the user's message into %s. Reply with ONLY the "
            "translation, no preamble, no explanation.", xlang);
        char tr[1024] = "";
        if (bsdr_llm_run_ex(&llm, sysx, text, NULL, NULL, NULL, NULL, 0, NULL, tr, sizeof tr) && tr[0])
            bsdr_app_tts_say(app, tr);
        BSDR_INFO("bsdr.roomcmd", "translated -> %s: %.60s", xlang, tr);
        return;
    }

}

static void worker_fn(void *arg) {
    bsdr_roomcmd *rc = (bsdr_roomcmd *)arg;
    bsdr_mutex_lock(rc->lock);
    while (!rc->stop) {
        rc_job *j = rc->q_head;
        if (!j) { rc->overloaded = 0; bsdr_cond_wait(rc->cv, rc->lock); continue; }
        rc->q_head = j->next; if (!rc->q_head) rc->q_tail = NULL; rc->q_len--;
        bsdr_mutex_unlock(rc->lock);
        process_job(rc, j);
        free(j->pcm); free(j);
        bsdr_mutex_lock(rc->lock);
    }
    bsdr_mutex_unlock(rc->lock);
}

void bsdr_roomcmd_free(bsdr_roomcmd *rc) {
    if (!rc) return;
    bsdr_mutex_lock(rc->lock);
    rc->stop = 1;
    bsdr_cond_signal(rc->cv);
    bsdr_mutex_unlock(rc->lock);
    if (rc->worker) bsdr_thread_join(rc->worker);
    for (rc_job *j = rc->q_head; j; ) { rc_job *n = j->next; free(j->pcm); free(j); j = n; }
    for (int i = 0; i < RC_SLOTS; i++) free(rc->slots[i].buf);
    if (rc->cv) bsdr_cond_free(rc->cv);
    if (rc->lock) bsdr_mutex_free(rc->lock);
    free(rc);
}
