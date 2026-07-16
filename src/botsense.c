/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* botsense — see include/bsdr/botsense.h. The mechanical VAD/segmentation half of the old roomcmd,
 * extracted so a bot plugin gets whole utterances on a host worker thread. */
#include "bsdr/botsense.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <stdlib.h>
#include <string.h>

#define TAG "bsdr.botsense"
#define BS_RATE       48000
#define BS_MAX_SEC    10
#define BS_SILENCE_MS 800     /* end an utterance this long after speech stops */
#define BS_MIN_MS     350     /* ignore blips shorter than this */
#define BS_SLOTS      8       /* concurrent speakers we track VAD for */
#define BS_QUEUE_MAX  6

typedef struct {
    uint32_t ssrc;
    int      channels;
    int16_t *buf;
    size_t   count, cap;
    int      speaking, silence_ms;
    float    noise;
    int      active;
} bs_slot;

typedef struct bs_job {
    uint32_t ssrc;
    int      channels, frames;
    int16_t *pcm;            /* owned */
    struct bs_job *next;
} bs_job;

struct bsdr_botsense {
    bsdr_mutex *lock;
    bsdr_cond  *cv;
    bsdr_thread *worker;
    volatile int stop;
    bs_slot     slots[BS_SLOTS];
    bs_job     *q_head, *q_tail;
    int         q_len;
    bsdr_utterance_cb cb;
    void       *user;
    int         in_cb;       /* set while the worker is inside cb() — unsubscribe waits on it */
};

static float mean_abs(const int16_t *p, size_t n) {
    if (!n) return 0;
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i] < 0 ? -p[i] : p[i];
    return (float)s / (float)n;
}

/* find/allocate the VAD slot for an SSRC (caller holds the lock) */
static bs_slot *slot_for(bsdr_botsense *b, uint32_t ssrc, int channels) {
    bs_slot *lru = &b->slots[0];
    for (int i = 0; i < BS_SLOTS; i++) {
        if (b->slots[i].active && b->slots[i].ssrc == ssrc) return &b->slots[i];
        if (!b->slots[i].active) lru = &b->slots[i];
    }
    if (lru->buf) lru->count = 0;
    lru->ssrc = ssrc; lru->channels = channels; lru->count = 0;
    lru->speaking = 0; lru->silence_ms = 0; lru->noise = 200.0f; lru->active = 1;
    if (!lru->buf) {
        lru->cap = (size_t)BS_RATE * BS_MAX_SEC * (channels > 0 ? channels : 1);
        lru->buf = malloc(lru->cap * sizeof(int16_t));
        if (!lru->buf) { lru->active = 0; return NULL; }
    }
    return lru;
}

/* queue a finished utterance for the worker (caller holds the lock); resets the slot */
static void enqueue(bsdr_botsense *b, bs_slot *s) {
    int frames = (int)(s->count / (s->channels > 0 ? s->channels : 1));
    int drop = (frames < BS_RATE * BS_MIN_MS / 1000) || b->q_len >= BS_QUEUE_MAX || !b->cb;
    bs_job *j = drop ? NULL : calloc(1, sizeof *j);
    if (j) {
        j->pcm = malloc(s->count * sizeof(int16_t));
        if (!j->pcm) { free(j); j = NULL; }
    }
    if (j) {
        memcpy(j->pcm, s->buf, s->count * sizeof(int16_t));
        j->ssrc = s->ssrc; j->channels = s->channels; j->frames = frames;
        if (b->q_tail) b->q_tail->next = j; else b->q_head = j;
        b->q_tail = j; b->q_len++;
        bsdr_cond_signal(b->cv);
    }
    s->count = 0; s->speaking = 0; s->silence_ms = 0;
}

void bsdr_botsense_tap(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user) {
    bsdr_botsense *b = (bsdr_botsense *)user;
    if (!b || b->stop || !pcm || frames <= 0) return;
    size_t n = (size_t)frames * (channels > 0 ? channels : 1);
    int ms = frames * 1000 / BS_RATE;
    float lvl = mean_abs(pcm, n);
    bsdr_mutex_lock(b->lock);
    bs_slot *s = slot_for(b, ssrc, channels);
    if (!s) { bsdr_mutex_unlock(b->lock); return; }
    if (!s->speaking) s->noise = s->noise * 0.95f + lvl * 0.05f;   /* adapt only on silence */
    float thresh = s->noise * 3.0f + 300.0f;
    int voiced = lvl > thresh;
    if (voiced) { s->speaking = 1; s->silence_ms = 0; }
    else if (s->speaking) s->silence_ms += ms;
    if (s->speaking && s->count + n <= s->cap) { memcpy(s->buf + s->count, pcm, n * sizeof(int16_t)); s->count += n; }
    if (s->speaking && (s->silence_ms >= BS_SILENCE_MS || s->count + n > s->cap)) enqueue(b, s);
    bsdr_mutex_unlock(b->lock);
}

static void worker_fn(void *arg) {
    bsdr_botsense *b = (bsdr_botsense *)arg;
    bsdr_mutex_lock(b->lock);
    while (!b->stop) {
        if (!b->q_head) { bsdr_cond_wait_ms(b->cv, b->lock, 250); continue; }
        bs_job *j = b->q_head;
        b->q_head = j->next; if (!b->q_head) b->q_tail = NULL;
        b->q_len--;
        bsdr_utterance_cb cb = b->cb; void *user = b->user;
        if (cb) b->in_cb = 1;
        bsdr_mutex_unlock(b->lock);

        if (cb) cb(j->ssrc, j->pcm, j->frames, j->channels, user);   /* STT + loop happen here */
        free(j->pcm); free(j);

        bsdr_mutex_lock(b->lock);
        b->in_cb = 0;
        bsdr_cond_signal(b->cv);   /* let a waiting unsubscribe/free proceed */
    }
    bsdr_mutex_unlock(b->lock);
}

bsdr_botsense *bsdr_botsense_new(void) {
    bsdr_botsense *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->lock = bsdr_mutex_new();
    b->cv   = bsdr_cond_new();
    if (!b->lock || !b->cv) { bsdr_botsense_free(b); return NULL; }
    b->worker = bsdr_thread_start(worker_fn, b);
    if (!b->worker) { bsdr_botsense_free(b); return NULL; }
    BSDR_INFO(TAG, "utterance segmenter up");
    return b;
}

void bsdr_botsense_set_cb(bsdr_botsense *b, bsdr_utterance_cb cb, void *user) {
    if (!b) return;
    bsdr_mutex_lock(b->lock);
    b->cb = cb; b->user = user;
    /* If clearing, wait for any in-flight callback to finish so the owner can be unloaded safely. */
    if (!cb) while (b->in_cb) bsdr_cond_wait_ms(b->cv, b->lock, 100);
    bsdr_mutex_unlock(b->lock);
}

int bsdr_botsense_has_cb(bsdr_botsense *b) {
    if (!b) return 0;
    bsdr_mutex_lock(b->lock);
    int has = b->cb != NULL;
    bsdr_mutex_unlock(b->lock);
    return has;
}

void bsdr_botsense_free(bsdr_botsense *b) {
    if (!b) return;
    if (b->worker) {
        bsdr_mutex_lock(b->lock);
        b->stop = 1; b->cb = NULL;
        bsdr_cond_signal(b->cv);
        bsdr_mutex_unlock(b->lock);
        bsdr_thread_join(b->worker);
    }
    for (bs_job *j = b->q_head; j; ) { bs_job *nx = j->next; free(j->pcm); free(j); j = nx; }
    for (int i = 0; i < BS_SLOTS; i++) free(b->slots[i].buf);
    if (b->cv) bsdr_cond_free(b->cv);
    if (b->lock) bsdr_mutex_free(b->lock);
    free(b);
}
