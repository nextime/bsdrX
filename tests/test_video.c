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
/* Video RTP/SRTP core test (no headset, no display). Requires BSDR_ENABLE_VIDEO.
 *
 * Sends an H.264 access unit (a small NAL + a large NAL that must FU-A fragment)
 * through the real SRTP sender over loopback UDP; a receiver SRTP-unprotects,
 * RTP-depacketizes, and we compare the reassembled NALs to the originals.
 * Exercises DTLS-keyed SRTP protect/unprotect + RFC 6184 packetization.
 */
#include "bsdr/platform.h"
#include "bsdr/net.h"
#include "bsdr/log.h"
#include "bsdr/udp_transport.h"
#include "bsdr/video.h"

#include <srtp2/srtp.h>
#include <stdio.h>
#include <string.h>

#define PORT_TX 45122
#define PORT_RX 45123
#define SSRC    0x42425344u
#define PT      96

int main(void) {
    int failures = 0;
    bsdr_log_set_level(BSDR_LOG_WARN);
    if (!bsdr_platform_init()) { printf("platform init failed\n"); return 1; }

    /* shared SRTP master key (in the app this comes from the DTLS handshake) */
    bsdr_srtp_keys keys;
    memset(&keys, 0, sizeof(keys));
    keys.profile = BSDR_SRTP_AES128_CM_SHA1_80;
    keys.send_master_len = 30;
    for (int i = 0; i < 30; i++) keys.send_master[i] = (uint8_t)(i + 1);

    bsdr_udp tx, rx;
    bsdr_udp_open(&tx, PORT_TX, "127.0.0.1", PORT_RX);
    bsdr_udp_open(&rx, PORT_RX, "127.0.0.1", PORT_TX);

    bsdr_video_sender *vs = bsdr_video_sender_new(&tx, &keys, PT, SSRC);
    if (!vs) { printf("FAIL sender_new\n"); return 1; }

    /* receiver SRTP session (inbound), same key */
    srtp_t recv_srtp;
    srtp_policy_t pol;
    memset(&pol, 0, sizeof(pol));
    srtp_crypto_policy_set_rtp_default(&pol.rtp);
    srtp_crypto_policy_set_rtcp_default(&pol.rtcp);
    pol.ssrc.type = ssrc_specific;
    pol.ssrc.value = SSRC;
    pol.key = keys.send_master;
    if (srtp_create(&recv_srtp, &pol) != srtp_err_status_ok) { printf("FAIL recv srtp\n"); return 1; }

    /* build an access unit: NAL #1 small (8B), NAL #2 large (3000B) */
    static uint8_t au[8000];
    size_t n = 0;
    uint8_t sc[4] = { 0, 0, 0, 1 };
    uint8_t nal1[8] = { 0x67, 1, 2, 3, 4, 5, 6, 7 };           /* type 7 (SPS-ish) */
    memcpy(au + n, sc, 4); n += 4; memcpy(au + n, nal1, 8); n += 8;
    size_t nal2_len = 3000;
    memcpy(au + n, sc, 4); n += 4;
    size_t nal2_off = n;
    au[n++] = 0x65;                                            /* type 5 (IDR) */
    for (size_t i = 1; i < nal2_len; i++) au[n++] = (uint8_t)(i & 0xff);

    if (bsdr_video_send_access_unit(vs, au, n, 3000) != 0) { printf("FAIL send_au\n"); failures++; }

    /* receive + depacketize */
    bsdr_h264_depay *dp = bsdr_h264_depay_new();
    static uint8_t out[16000];
    size_t outlen = 0;
    int pkts = 0, marker_seen = 0;
    for (;;) {
        uint8_t buf[2048];
        int r = bsdr_udp_recv(&rx, buf, sizeof(buf), 500);
        if (r <= 0) break;
        int len = r;
        if (srtp_unprotect(recv_srtp, buf, &len) != srtp_err_status_ok) {
            printf("FAIL srtp_unprotect\n"); failures++; break;
        }
        pkts++;
        if (buf[1] & 0x80) marker_seen = 1;                   /* marker bit */
        bsdr_h264_depay_feed(dp, buf + 12, (size_t)len - 12, out, sizeof(out), &outlen);
    }
    if (pkts >= 2) printf("PASS srtp_roundtrip (%d packets)\n", pkts);
    else { printf("FAIL srtp_roundtrip (%d packets)\n", pkts); failures++; }
    if (marker_seen) printf("PASS rtp_marker_on_last\n");
    else { printf("FAIL rtp_marker_on_last\n"); failures++; }

    /* reassembled stream must equal the original access unit (both Annex-B) */
    if (outlen == n && memcmp(out, au, n) == 0)
        printf("PASS nal_reassembly (%zu bytes, incl. FU-A of 3000B NAL)\n", outlen);
    else { printf("FAIL nal_reassembly (got %zu want %zu)\n", outlen, n); failures++; }

    (void)nal2_off;
    bsdr_h264_depay_free(dp);
    bsdr_video_sender_free(vs);
    srtp_dealloc(recv_srtp);
    bsdr_udp_close(&tx); bsdr_udp_close(&rx);
    bsdr_platform_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nOK - video RTP/SRTP core passed\n", failures);
    return failures ? 1 : 0;
}
