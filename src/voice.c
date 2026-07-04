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
/* Voice assistant orchestrator. */
#include "bsdr/voice.h"
#include "bsdr/compcontrol.h"
#include "bsdr/json.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICE_RATE  48000
#define CAP_SECONDS 120                       /* capture-buffer ceiling (memory bound) */
#define CAP_SAMPLES (VOICE_RATE * CAP_SECONDS) /* mono; the owner mic is mono */

struct bsdr_voice {
    bsdr_voice_config cfg;
    bsdr_compcontrol *cc;
    bsdr_injector *inj;
    bsdr_mutex *lock;
    int16_t *buf;          /* interleaved capture */
    size_t count;          /* samples buffered */
    int channels;
    volatile int recording;         /* buffering mic PCM (LISTENING) */
    volatile int state;             /* bsdr_voice_state */
    volatile int send_req, cancel_req;  /* CONFIRM resolution */
    volatile int abort_req;         /* stop the running command loop (WORKING) */
    volatile int shutdown;          /* free() asked the worker to exit */
    bsdr_voice_mode mode;
    bsdr_thread *worker;
    bsdr_voice_state_cb state_cb;      void *state_user;
    bsdr_voice_feedback_cb fb_cb;      void *fb_user;
    bsdr_voice_shot_cb shot_cb;        void *shot_user;
    char last[2048];
};

/* Fill in sane defaults for any unset knob. */
static void apply_defaults(bsdr_voice_config *c) {
    if (c->start_ms   <= 0) c->start_ms   = 4000;    /* wait up to 4 s for speech */
    if (c->silence_ms <= 0) c->silence_ms = 900;     /* end 0.9 s after speech stops */
    if (c->max_ms     <= 0) c->max_ms     = 300000;  /* listening ceiling: 5 min */
    if (c->confirm_ms <= 0) c->confirm_ms = 60000;   /* Send/Cancel auto-cancels after 1 min */
}

bsdr_voice *bsdr_voice_new(const bsdr_voice_config *cfg, bsdr_injector *inj) {
    bsdr_voice *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->cfg = *cfg;
    apply_defaults(&v->cfg);
    v->inj = inj;
    v->cc = bsdr_compcontrol_new(inj);
    v->lock = bsdr_mutex_new();
    v->buf = malloc(CAP_SAMPLES * sizeof(int16_t));   /* ~11 MB */
    if (!v->buf) { bsdr_voice_free(v); return NULL; }  /* else push_pcm/phase_* deref NULL */
    v->channels = 1;
    return v;
}

void bsdr_voice_update_config(bsdr_voice *v, const bsdr_voice_config *cfg) {
    bsdr_mutex_lock(v->lock);
    v->cfg = *cfg;
    apply_defaults(&v->cfg);
    bsdr_mutex_unlock(v->lock);
}

void bsdr_voice_set_state_cb(bsdr_voice *v, bsdr_voice_state_cb cb, void *user) {
    v->state_cb = cb; v->state_user = user;
}
void bsdr_voice_set_feedback_cb(bsdr_voice *v, bsdr_voice_feedback_cb cb, void *user) {
    v->fb_cb = cb; v->fb_user = user;
}
void bsdr_voice_set_shot_cb(bsdr_voice *v, bsdr_voice_shot_cb cb, void *user) {
    v->shot_cb = cb; v->shot_user = user;
}

int  bsdr_voice_state_get(bsdr_voice *v) { return v->state; }
bool bsdr_voice_busy(bsdr_voice *v) { return v->state != BSDR_VST_IDLE; }

void bsdr_voice_stop_capture(bsdr_voice *v) {
    if (v->state == BSDR_VST_LISTENING) v->recording = 0;   /* VAD loop ends -> CONFIRM */
}
void bsdr_voice_confirm(bsdr_voice *v, bool send) {
    if (v->state != BSDR_VST_CONFIRM) return;
    if (send) v->send_req = 1; else v->cancel_req = 1;
}
void bsdr_voice_abort(bsdr_voice *v) {
    if (v->state == BSDR_VST_WORKING) v->abort_req = 1;   /* stop the command loop */
}

void bsdr_voice_push_pcm(bsdr_voice *v, const int16_t *pcm, int frames, int channels) {
    if (!v->recording) return;
    bsdr_mutex_lock(v->lock);
    v->channels = channels;
    size_t n = (size_t)frames * channels;
    if (v->count + n <= CAP_SAMPLES) {
        memcpy(v->buf + v->count, pcm, n * sizeof(int16_t));
        v->count += n;
    }
    bsdr_mutex_unlock(v->lock);
}

void bsdr_voice_last(bsdr_voice *v, char *out, size_t len) {
    bsdr_mutex_lock(v->lock);
    snprintf(out, len, "%s", v->last);
    bsdr_mutex_unlock(v->lock);
}

/* Set the status line and mirror it to the feedback sink (balloon bubble/history). */
static void set_last(bsdr_voice *v, const char *s) {
    bsdr_mutex_lock(v->lock);
    snprintf(v->last, sizeof(v->last), "%s", s);
    bsdr_mutex_unlock(v->lock);
    if (v->fb_cb) v->fb_cb(v->fb_user, s);
}
static void set_state(bsdr_voice *v, int st) {
    v->state = st;
    if (v->state_cb) v->state_cb(v->state_user, st);
}

/* Integer sqrt (no libm dependency). */
static uint32_t isqrt64(uint64_t x) {
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > x) b >>= 2;
    while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (uint32_t)r;
}

/* RMS of a mono/interleaved int16 chunk (all channels summed in — good enough
 * for a voice-activity gate). */
static int chunk_rms(const int16_t *p, size_t n) {
    if (!n) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (int64_t)p[i] * p[i];
    return (int)isqrt64((uint64_t)(sum / (int64_t)n));
}

/* Phase 1: capture until silence / stop / the listening ceiling. Returns true if
 * usable speech was captured. */
static bool phase_listen(bsdr_voice *v) {
    bsdr_mutex_lock(v->lock);
    v->count = 0;
    int start_ms = v->cfg.start_ms, silence_ms = v->cfg.silence_ms, max_ms = v->cfg.max_ms;
    bsdr_mutex_unlock(v->lock);
    v->recording = 1;
    set_state(v, BSDR_VST_LISTENING);
    set_last(v, "listening... (click the balloon to stop)");

    const int STEP_MS = 40;
    size_t analyzed = 0;
    int elapsed = 0, silence_run = 0;
    int64_t floor_sum = 0; int floor_n = 0; int noise_floor = 200;
    bool speech = false;
    static int16_t tmp[VOICE_RATE / 10];   /* 100 ms scratch */
    while (v->recording && !v->shutdown) {
        bsdr_sleep_ms(STEP_MS);
        elapsed += STEP_MS;
        bsdr_mutex_lock(v->lock);
        size_t count = v->count;
        size_t navail = count > analyzed ? count - analyzed : 0;
        if (navail > sizeof(tmp) / sizeof(tmp[0])) navail = sizeof(tmp) / sizeof(tmp[0]);
        if (navail) { memcpy(tmp, v->buf + analyzed, navail * sizeof(int16_t)); analyzed += navail; }
        bsdr_mutex_unlock(v->lock);

        int rms = chunk_rms(tmp, navail);
        if (elapsed <= 250 && navail) { floor_sum += rms; floor_n++;
            noise_floor = floor_n ? (int)(floor_sum / floor_n) : noise_floor; }
        int thresh = noise_floor * 3 + 250;
        bool voiced = navail && rms > thresh;
        if (voiced) { speech = true; silence_run = 0; }
        else if (speech) silence_run += STEP_MS;

        if (speech && silence_run >= silence_ms) break;   /* utterance complete */
        if (!speech && elapsed >= start_ms) break;        /* nobody spoke */
        if (elapsed >= max_ms) { BSDR_INFO("bsdr.voice", "listening ceiling (%d ms) hit", max_ms); break; }
    }
    v->recording = 0;
    bsdr_mutex_lock(v->lock);
    size_t count = v->count;
    bsdr_mutex_unlock(v->lock);
    return speech && count >= (size_t)VOICE_RATE / 5;
}

/* Phase 2: wait for Send / Cancel (auto-cancel after confirm_ms). Returns true=send. */
static bool phase_confirm(bsdr_voice *v) {
    v->send_req = v->cancel_req = 0;
    set_state(v, BSDR_VST_CONFIRM);
    set_last(v, "ready — tap Send to run, Cancel to discard");
    int waited = 0, timeout = v->cfg.confirm_ms;
    while (!v->shutdown) {
        if (v->send_req) return true;
        if (v->cancel_req) { set_last(v, "cancelled"); return false; }
        if (waited >= timeout) { set_last(v, "cancelled (timed out)"); return false; }
        bsdr_sleep_ms(50); waited += 50;
    }
    return false;
}

/* Phase 3: transcribe + act. */
static void phase_work(bsdr_voice *v) {
    v->abort_req = 0;
    set_state(v, BSDR_VST_WORKING);
    bsdr_mutex_lock(v->lock);
    size_t count = v->count; int ch = v->channels;
    bsdr_mutex_unlock(v->lock);

    set_last(v, "transcribing...");
    char text[2048] = "";
    if (!bsdr_stt_transcribe(&v->cfg.stt, v->buf, (int)(count / ch), VOICE_RATE, ch,
                             text, sizeof(text)) || !text[0]) {
        set_last(v, "(transcription failed)"); return;
    }
    BSDR_INFO("bsdr.voice", "heard: %s", text);
    set_last(v, text);                                  /* show what was heard */

    if (v->mode == BSDR_VOICE_DICTATE) {
        char res[64], args[2176], esc[2100];
        bsdr_json_escape(esc, sizeof(esc), text);
        snprintf(args, sizeof(args), "{\"text\":\"%s\"}", esc);
        bsdr_compcontrol_exec("type_text", args, res, sizeof(res), v->cc);
    } else {
        char reply[2048] = "";
        set_last(v, "thinking...");
        /* On-demand vision: offer a take_screenshot tool the model calls only when the
         * request needs it (same signature as the shot cb, passed straight through). */
        bsdr_llm_image_cb icb = (v->cfg.vision && v->shot_cb) ? (bsdr_llm_image_cb)v->shot_cb : NULL;
        bool ok = bsdr_llm_run_ex(&v->cfg.llm, v->cfg.system_prompt, text,
                                  bsdr_compcontrol_exec, v->cc, icb, v->shot_user,
                                  &v->abort_req, reply, sizeof(reply));
        set_last(v, v->abort_req ? "stopped" : (ok ? (reply[0] ? reply : "done") : "(assistant unavailable)"));
    }
}

static void voice_cycle(bsdr_voice *v) {
    if (!phase_listen(v)) { set_last(v, "(no speech)"); goto idle; }
    if (!phase_confirm(v)) goto idle;   /* cancelled / timed out */
    phase_work(v);
idle:
    set_state(v, BSDR_VST_IDLE);
}

static void worker(void *arg) {
    voice_cycle((bsdr_voice *)arg);
}

void bsdr_voice_trigger(bsdr_voice *v, bsdr_voice_mode mode) {
    if (v->state != BSDR_VST_IDLE) return;    /* a cycle is already in flight */
    if (v->worker) { bsdr_thread_join(v->worker); v->worker = NULL; }
    v->state = BSDR_VST_LISTENING;            /* claim immediately (guards re-entry) */
    v->mode = mode;
    v->worker = bsdr_thread_start(worker, v);
}

void bsdr_voice_free(bsdr_voice *v) {
    if (!v) return;
    v->shutdown = 1; v->recording = 0;        /* break any active phase loop */
    if (v->worker) bsdr_thread_join(v->worker);
    bsdr_compcontrol_free(v->cc);
    bsdr_mutex_free(v->lock);
    free(v->buf);
    free(v);
}
