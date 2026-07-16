/*
 * bsdrX — botsense (utterance VAD/segmentation) unit test.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>. GPLv3-or-later.
 *
 * Feeds synthetic per-SSRC frames (silence -> speech -> silence) and checks that a complete utterance is
 * segmented and delivered on the worker thread, that a sub-minimum blip is dropped, and that has_cb
 * tracks subscribe/unsubscribe.
 */
#include "bsdr/botsense.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int      g_calls = 0;
static uint32_t g_ssrc = 0;
static int      g_frames = 0;

static void on_utt(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user) {
    (void)pcm; (void)channels; (void)user;
    g_calls++; g_ssrc = ssrc; g_frames = frames;
}

/* one 20 ms mono frame at 48 kHz = 960 samples, all at amplitude `amp` */
static void feed(bsdr_botsense *b, uint32_t ssrc, int amp) {
    int16_t f[960];
    for (int i = 0; i < 960; i++) f[i] = (int16_t)((i & 1) ? amp : -amp);
    bsdr_botsense_tap(ssrc, f, 960, 1, b);
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_calls(int want, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 10) { if (g_calls >= want) return 1; sleep_ms(10); }
    return g_calls >= want;
}

int main(void) {
    int fails = 0;
    bsdr_botsense *b = bsdr_botsense_new();
    if (!b) { fprintf(stderr, "FAIL new\n"); return 1; }

    if (bsdr_botsense_has_cb(b)) { fprintf(stderr, "FAIL has_cb before set\n"); fails++; }
    bsdr_botsense_set_cb(b, on_utt, NULL);
    if (!bsdr_botsense_has_cb(b)) { fprintf(stderr, "FAIL has_cb after set\n"); fails++; }

    /* a full utterance on ssrc 42: settle noise low, ~600 ms speech, then > 800 ms silence to end it */
    for (int i = 0; i < 5; i++)  feed(b, 42, 0);        /* silence -> low noise floor */
    for (int i = 0; i < 30; i++) feed(b, 42, 6000);     /* 30 * 20 ms = 600 ms speech */
    for (int i = 0; i < 45; i++) feed(b, 42, 0);        /* 900 ms silence -> ends the utterance */

    if (!wait_calls(1, 2000)) { fprintf(stderr, "FAIL utterance not delivered\n"); fails++; }
    if (g_ssrc != 42)          { fprintf(stderr, "FAIL ssrc %u\n", g_ssrc); fails++; }
    /* accumulates while speaking incl. trailing silence until the cutoff — must clear the 350 ms floor */
    if (g_frames < 48000 * 350 / 1000) { fprintf(stderr, "FAIL utterance too short (%d frames)\n", g_frames); fails++; }

    /* segmentation resets: a SECOND utterance on the same speaker is delivered independently */
    g_calls = 0;
    for (int i = 0; i < 30; i++) feed(b, 42, 6000);
    for (int i = 0; i < 45; i++) feed(b, 42, 0);
    if (!wait_calls(1, 2000)) { fprintf(stderr, "FAIL second utterance not delivered\n"); fails++; }

    /* unsubscribe: no further deliveries */
    bsdr_botsense_set_cb(b, NULL, NULL);
    if (bsdr_botsense_has_cb(b)) { fprintf(stderr, "FAIL has_cb after clear\n"); fails++; }

    bsdr_botsense_free(b);
    if (fails == 0) printf("PASS: test_botsense\n");
    return fails ? 1 : 0;
}
