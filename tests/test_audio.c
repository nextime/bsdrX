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
/* Opus RTP/SRTP audio core loopback (no audio server needed). BSDR_ENABLE_AUDIO.
 * PCM (sine) -> Opus encode -> RTP -> SRTP -> unprotect -> Opus decode -> PCM.
 * Verifies the encode/packetize/protect/decode roundtrip and that real signal
 * (not silence) comes out. */
#include "bsdr/platform.h"
#include "bsdr/protocol.h"
#include "bsdr/udp_transport.h"
#include "bsdr/log.h"
#include "bsdr/audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PORT_TX 45152
#define PORT_RX 45153
#define SSRC    0x42425341u
#define PT      97
#define CH      2

static int g_frames = 0;
static double g_energy = 0;
static void pcm_cb(const int16_t *pcm, int frames, int ch, void *u) {
    (void)u;
    g_frames += frames;
    for (int i = 0; i < frames * ch; i++) g_energy += fabs((double)pcm[i]);
}

int main(void) {
    int failures = 0;
    bsdr_log_set_level(BSDR_LOG_WARN);
    if (!bsdr_platform_init()) return 1;

    /* shared SRTP master (from a real DTLS handshake in the app) */
    bsdr_srtp_keys keys;
    memset(&keys, 0, sizeof(keys));
    keys.profile = BSDR_SRTP_AES128_CM_SHA1_80;
    keys.send_master_len = keys.recv_master_len = 30;
    for (int i = 0; i < 30; i++) keys.send_master[i] = keys.recv_master[i] = (uint8_t)(i + 3);

    bsdr_udp tx, rx;
    bsdr_udp_open(&tx, PORT_TX, "127.0.0.1", PORT_RX);
    bsdr_udp_open(&rx, PORT_RX, "127.0.0.1", PORT_TX);

    bsdr_audio_sender *s = bsdr_audio_sender_new(&tx, &keys, PT, SSRC, CH, BSDR_AUDIO_DESKTOP_BPS);
    bsdr_audio_recv *r = bsdr_audio_recv_new(&keys, PT, SSRC, CH, pcm_cb, NULL);
    if (!s || !r) { printf("FAIL setup\n"); return 1; }

    /* a 440 Hz sine, stereo, in 20 ms (960-sample) frames */
    const int N = 12;
    int16_t frame[BSDR_OPUS_FRAME * CH];
    int sent = 0;
    double phase = 0, dp = 2 * M_PI * 440.0 / BSDR_AUDIO_CLOCK_HZ;
    for (int f = 0; f < N; f++) {
        for (int i = 0; i < BSDR_OPUS_FRAME; i++) {
            int16_t v = (int16_t)(8000 * sin(phase)); phase += dp;
            frame[2*i] = v; frame[2*i+1] = v;
        }
        if (bsdr_audio_send_pcm(s, frame, BSDR_OPUS_FRAME) == 0) sent++;
    }
    if (sent == N) printf("PASS opus_encode_send (%d frames)\n", sent);
    else { printf("FAIL opus_encode_send (%d/%d)\n", sent, N); failures++; }

    int pkts = 0;
    for (;;) {
        uint8_t buf[2048];
        int n = bsdr_udp_recv(&rx, buf, sizeof(buf), 400);
        if (n <= 0) break;
        if (bsdr_audio_recv_feed(r, buf, n) > 0) pkts++;
    }
    if (pkts >= N - 1) printf("PASS srtp_opus_roundtrip (%d pkts decoded)\n", pkts);
    else { printf("FAIL srtp_opus_roundtrip (%d/%d)\n", pkts, N); failures++; }

    double avg = g_frames ? g_energy / (g_frames * CH) : 0;
    if (g_frames > 0 && avg > 1000) printf("PASS decoded_signal (%d samples, avg|amp|=%.0f)\n", g_frames, avg);
    else { printf("FAIL decoded_signal (frames=%d avg=%.0f)\n", g_frames, avg); failures++; }

    bsdr_audio_sender_free(s);
    bsdr_audio_recv_free(r);
    bsdr_udp_close(&tx); bsdr_udp_close(&rx);
    bsdr_platform_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nOK - Opus RTP/SRTP audio core passed\n", failures);
    return failures ? 1 : 0;
}
