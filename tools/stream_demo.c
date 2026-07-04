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
/* End-to-end streamer demo, no headset needed (needs DISPLAY).
 *
 * Captures the real desktop, H.264-encodes (NVENC), and streams it over a real
 * DTLS handshake as SRTP — to an in-process "headset" that unprotects, RFC 6184
 * depacketizes, and DECODES the H.264 frames. Proves the whole video path the
 * agent uses: capture -> encode -> RTP -> SRTP(DTLS keys) -> recv -> decode.
 *
 *   DISPLAY=:0 ./stream_demo [seconds]
 */
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/video.h"
#include "bsdr/capture.h"
#include "bsdr/filesrc.h"

#include <srtp2/srtp.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <string.h>

#define PORT_A 45142
#define PORT_H 45143
#define SSRC   0x42425344u

static volatile int g_stop = 0;
static int g_decoded = 0, g_pkts = 0;

/* headset: DTLS server, then receive SRTP video + decode. */
static void headset(void *arg) {
    (void)arg;
    bsdr_udp uh;
    bsdr_udp_open(&uh, PORT_H, NULL, 0);
    bsdr_dtls *dh = bsdr_dtls_new(&uh, BSDR_DTLS_SERVER);
    if (!bsdr_dtls_handshake(dh, 8000, NULL)) { bsdr_dtls_free(dh); bsdr_udp_close(&uh); return; }

    bsdr_srtp_keys k;
    bsdr_dtls_export_srtp(dh, &k);
    srtp_policy_t pol; memset(&pol, 0, sizeof(pol));
    if (k.profile == BSDR_SRTP_AEAD_AES_128_GCM) {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtcp);
    } else {
        srtp_crypto_policy_set_rtp_default(&pol.rtp);
        srtp_crypto_policy_set_rtcp_default(&pol.rtcp);
    }
    pol.ssrc.type = ssrc_specific; pol.ssrc.value = SSRC; pol.key = k.recv_master;
    srtp_t rx; srtp_create(&rx, &pol);

    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *dec = avcodec_alloc_context3(dc);
    avcodec_open2(dec, dc, NULL);
    AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
    bsdr_h264_depay *dp = bsdr_h264_depay_new();
    static uint8_t au[2*1024*1024]; size_t aulen = 0;

    while (!g_stop) {
        uint8_t buf[2048];
        int r = bsdr_udp_recv(&uh, buf, sizeof(buf), 100);
        if (r < 14) continue;                          /* runt / not media */
        if (buf[0] < 128 || buf[0] >= 192) continue;   /* not SRTP/RTP */
        int len = r;
        if (srtp_unprotect(rx, buf, &len) != srtp_err_status_ok || len < 12) continue;
        g_pkts++;
        int marker = buf[1] & 0x80;
        bsdr_h264_depay_feed(dp, buf+12, (size_t)len-12, au, sizeof(au), &aulen);
        if (marker) {                                  /* end of access unit */
            if (aulen) {
                pkt->data = au; pkt->size = (int)aulen;
                if (avcodec_send_packet(dec, pkt) == 0)
                    while (avcodec_receive_frame(dec, fr) == 0) { g_decoded++; av_frame_unref(fr); }
            }
            aulen = 0;                                 /* start next access unit */
        }
    }
    bsdr_h264_depay_free(dp);
    av_frame_free(&fr); av_packet_free(&pkt); avcodec_free_context(&dec);
    srtp_dealloc(rx); bsdr_dtls_free(dh); bsdr_udp_close(&uh);
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 3;
    const char *file = argc > 2 ? argv[2] : NULL;   /* file passthrough if given */
    bsdr_log_set_level(BSDR_LOG_INFO);
    if (!bsdr_platform_init()) return 1;
    bsdr_srtp_global_init();   /* init libsrtp before the receiver thread races */

    bsdr_thread *ht = bsdr_thread_start(headset, NULL);
    bsdr_sleep_ms(100);

    bsdr_udp ua;
    bsdr_udp_open(&ua, PORT_A, "127.0.0.1", PORT_H);
    bsdr_dtls *da = bsdr_dtls_new(&ua, BSDR_DTLS_CLIENT);
    if (!bsdr_dtls_handshake(da, 8000, NULL)) { fprintf(stderr, "dtls failed\n"); return 1; }
    bsdr_srtp_keys ka; bsdr_dtls_export_srtp(da, &ka);
    bsdr_video_sender *vs = bsdr_video_sender_new(&ua, &ka, 96, SSRC);

    bsdr_capture *cap = NULL;
    bsdr_filesrc *fsrc = NULL;
    if (file) {
        fsrc = bsdr_filesrc_open(file, false);
        if (!fsrc) { fprintf(stderr, "file open failed: %s\n", file); return 1; }
        int w,h; double fps; bsdr_filesrc_info(fsrc, &w, &h, &fps);
        printf("streaming FILE %s (%dx%d @%.1ffps, no re-encode) for up to %ds ...\n",
               file, w, h, fps, seconds);
    } else {
        bsdr_capture_config cfg = {0};
        cfg.fps = 15; cfg.bitrate = 4000000; cfg.out_width = 960; cfg.out_height = 540;
        cap = bsdr_capture_open(&cfg);
        if (!cap) { fprintf(stderr, "capture failed (DISPLAY?)\n"); return 1; }
        int w,h; const char *enc; bsdr_capture_info(cap, &w, &h, &enc);
        printf("streaming desktop %dx%d via %s for %ds ...\n", w, h, enc, seconds);
    }

    int sent = 0;
    uint64_t t0 = bsdr_now_ms();
    while (bsdr_now_ms() - t0 < (uint64_t)seconds * 1000) {
        const uint8_t *a; size_t l; uint32_t ts;
        int r = fsrc ? bsdr_filesrc_frame(fsrc, &a, &l, &ts)
                     : bsdr_capture_frame(cap, &a, &l, &ts);
        if (r > 0) { bsdr_video_send_access_unit(vs, a, l, ts); sent++; }
        else if (r < 0) break;   /* file EOF */
    }
    bsdr_sleep_ms(300);
    g_stop = 1;
    bsdr_thread_join(ht);

    if (cap) bsdr_capture_close(cap);
    if (fsrc) bsdr_filesrc_close(fsrc);
    bsdr_video_sender_free(vs);
    bsdr_dtls_free(da); bsdr_udp_close(&ua);
    bsdr_srtp_global_shutdown();
    bsdr_platform_cleanup();

    printf("\nsent %d frames -> headset received %d SRTP pkts, DECODED %d frames\n",
           sent, g_pkts, g_decoded);
    int ok = sent > 0 && g_decoded >= sent / 2;
    if (ok)
        printf("OK - %s streamed end-to-end (%s->SRTP/DTLS->depay->decode)\n",
               file ? "file" : "real desktop",
               file ? "demux(no re-encode)" : "capture->NVENC");
    else
        printf("FAILED\n");
    return ok ? 0 : 1;
}
