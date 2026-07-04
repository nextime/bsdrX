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
/* End-to-end media test with REAL DTLS-derived SRTP keys (no headset/display).
 * Requires BSDR_ENABLE_VIDEO.
 *
 * Two endpoints handshake over loopback UDP; BOTH export SRTP keys from their
 * own handshake (RFC 5764). The "agent" (DTLS client) SRTP-sends an H.264 access
 * unit; the "headset" (DTLS server) builds an inbound SRTP session from ITS
 * exported keys, unprotects, and depacketizes. If the agent's send-keys and the
 * headset's recv-keys match (they must, by construction) the packets decrypt and
 * the NALs reassemble — verifying bsdr_dtls_export_srtp + the SRTP video sender.
 */
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/video.h"

#include <srtp2/srtp.h>
#include <stdio.h>
#include <string.h>

#define PORT_A 45132
#define PORT_H 45133
#define SSRC   0x42425344u

struct hs { bsdr_dtls *d; volatile int ok; };
static void headset_handshake(void *arg) {
    struct hs *h = (struct hs *)arg;
    h->ok = bsdr_dtls_handshake(h->d, 8000, NULL) ? 1 : 0;
}

int main(void) {
    int failures = 0;
    bsdr_log_set_level(BSDR_LOG_WARN);
    if (!bsdr_platform_init()) { printf("platform init failed\n"); return 1; }
    /* libsrtp is initialized by the video sender (bsdr_video_sender_new). */

    bsdr_udp ua, uh;
    bsdr_udp_open(&ua, PORT_A, "127.0.0.1", PORT_H);
    bsdr_udp_open(&uh, PORT_H, NULL, 0);
    bsdr_dtls *da = bsdr_dtls_new(&ua, BSDR_DTLS_CLIENT);
    bsdr_dtls *dh = bsdr_dtls_new(&uh, BSDR_DTLS_SERVER);

    struct hs h = { dh, 0 };
    bsdr_thread *t = bsdr_thread_start(headset_handshake, &h);
    int aok = bsdr_dtls_handshake(da, 8000, NULL) ? 1 : 0;
    bsdr_thread_join(t);
    if (aok && h.ok) printf("PASS dtls_handshake\n");
    else { printf("FAIL dtls_handshake\n"); return 1; }

    bsdr_srtp_keys ka, kh;
    if (!bsdr_dtls_export_srtp(da, &ka) || !bsdr_dtls_export_srtp(dh, &kh)) {
        printf("FAIL srtp_key_export\n"); return 1;
    }
    /* the agent's send key must equal the headset's recv key */
    if (ka.send_master_len == kh.recv_master_len &&
        memcmp(ka.send_master, kh.recv_master, ka.send_master_len) == 0)
        printf("PASS srtp_keys_match (client send == server recv)\n");
    else { printf("FAIL srtp_keys_match\n"); failures++; }

    /* agent: SRTP video sender keyed from its own handshake (inits libsrtp) */
    bsdr_video_sender *vs = bsdr_video_sender_new(&ua, &ka, 96, SSRC);
    if (!vs) { printf("FAIL sender_new\n"); return 1; }

    /* headset: inbound SRTP session keyed from its own handshake */
    srtp_t rx;
    srtp_policy_t pol;
    memset(&pol, 0, sizeof(pol));
    if (kh.profile == BSDR_SRTP_AEAD_AES_128_GCM) {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtcp);
        printf("(negotiated SRTP profile: AEAD_AES_128_GCM)\n");
    } else {
        srtp_crypto_policy_set_rtp_default(&pol.rtp);
        srtp_crypto_policy_set_rtcp_default(&pol.rtcp);
        printf("(negotiated SRTP profile: AES_CM_128_HMAC_SHA1_80)\n");
    }
    pol.ssrc.type = ssrc_specific;
    pol.ssrc.value = SSRC;
    pol.key = kh.recv_master;
    if (srtp_create(&rx, &pol) != srtp_err_status_ok) { printf("FAIL rx srtp\n"); return 1; }

    /* an access unit: small NAL + a 2500B NAL (forces FU-A) */
    static uint8_t au[4000]; size_t n = 0;
    uint8_t sc[4] = {0,0,0,1};
    memcpy(au+n, sc, 4); n += 4; au[n++]=0x67; for (int i=0;i<7;i++) au[n++]=(uint8_t)i;
    memcpy(au+n, sc, 4); n += 4; au[n++]=0x65; for (int i=1;i<2500;i++) au[n++]=(uint8_t)(i&0xff);

    if (bsdr_video_send_access_unit(vs, au, n, 3000) != 0) { printf("FAIL send_au\n"); failures++; }

    bsdr_h264_depay *dp = bsdr_h264_depay_new();
    static uint8_t out[8000]; size_t outlen = 0; int pkts = 0;
    for (;;) {
        uint8_t buf[2048];
        int r = bsdr_udp_recv(&uh, buf, sizeof(buf), 500);
        if (r <= 0) break;
        int len = r;
        if (srtp_unprotect(rx, buf, &len) != srtp_err_status_ok) {
            printf("FAIL srtp_unprotect (key mismatch?)\n"); failures++; break;
        }
        pkts++;
        bsdr_h264_depay_feed(dp, buf+12, (size_t)len-12, out, sizeof(out), &outlen);
    }
    if (pkts >= 2 && outlen == n && memcmp(out, au, n) == 0)
        printf("PASS srtp_video_roundtrip (%d pkts, %zu bytes via real DTLS keys)\n", pkts, outlen);
    else { printf("FAIL srtp_video_roundtrip (pkts=%d outlen=%zu want=%zu)\n", pkts, outlen, n); failures++; }

    bsdr_h264_depay_free(dp);
    srtp_dealloc(rx);                /* before the sender shuts libsrtp down */
    bsdr_video_sender_free(vs);
    bsdr_dtls_free(da); bsdr_dtls_free(dh);
    bsdr_udp_close(&ua); bsdr_udp_close(&uh);
    bsdr_platform_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nOK - end-to-end SRTP video over real DTLS keys\n", failures);
    return failures ? 1 : 0;
}
