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
/* Capture the X11 desktop, H.264-encode (NVENC), and decode the result back to
 * prove the encoded stream is valid. Needs DISPLAY. BSDR_ENABLE_VIDEO.
 *
 *   DISPLAY=:0 ./video_capture_test [frames] [WxH]
 */
#include "bsdr/capture.h"
#include "bsdr/overlay.h"
#include "bsdr/log.h"

#include <libavcodec/avcodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void nal_types(const uint8_t *au, size_t len, char *out, size_t outcap) {
    out[0] = '\0';
    for (size_t i = 0; i + 4 < len; i++) {
        if (au[i] == 0 && au[i+1] == 0 &&
            ((au[i+2] == 1) || (au[i+2] == 0 && au[i+3] == 1))) {
            size_t h = au[i+2] == 1 ? i + 3 : i + 4;
            int t = au[h] & 0x1f;
            char b[8]; snprintf(b, sizeof(b), "%d ", t);
            if (strlen(out) + 4 < outcap) strcat(out, b);
        }
    }
}

int main(int argc, char **argv) {
    bsdr_log_set_level(BSDR_LOG_INFO);
    int frames = argc > 1 ? atoi(argv[1]) : 30;
    int w = 640, h = 360;
    if (argc > 2) sscanf(argv[2], "%dx%d", &w, &h);

    bsdr_capture_config cfg = {0};
    cfg.fps = 15;
    cfg.bitrate = 4000000;
    cfg.out_width = w;
    cfg.out_height = h;
    bsdr_capture *cap = bsdr_capture_open(&cfg);
    if (!cap) { fprintf(stderr, "FAIL: capture open (DISPLAY set?)\n"); return 1; }

    int ow, oh; const char *enc;
    bsdr_capture_info(cap, &ow, &oh, &enc);
    printf("capturing %dx%d via %s\n", ow, oh, enc);

    /* composite the in-VR overlay bar so we can confirm it lands in the video */
    bsdr_overlay *ov = bsdr_overlay_new();
    bsdr_capture_set_overlay(cap, ov);

    /* decoder to validate the encoded stream */
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *dec = avcodec_alloc_context3(dc);
    avcodec_open2(dec, dc, NULL);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();

    int got = 0, decoded = 0; size_t total = 0; int first_logged = 0;
    int bar_bright = -1;   /* bright icon pixels in the bar strip (last frame) */
    while (got < frames) {
        const uint8_t *au; size_t len; uint32_t ts;
        int r = bsdr_capture_frame(cap, &au, &len, &ts);
        if (r < 0) { fprintf(stderr, "capture EOF/err\n"); break; }
        if (r == 0) continue;
        got++; total += len;
        if (!first_logged) {
            char types[64]; nal_types(au, len, types, sizeof(types));
            printf("first AU: %zu bytes, NAL types: %s (expect 7=SPS 8=PPS 5=IDR)\n",
                   len, types);
            first_logged = 1;
        }
        /* decode-verify */
        pkt->data = (uint8_t *)au; pkt->size = (int)len;
        if (avcodec_send_packet(dec, pkt) == 0)
            while (avcodec_receive_frame(dec, fr) == 0) {
                decoded++;
                if (fr->data[0] && fr->height > 10) {   /* bright icon pixels in bar */
                    int b = 0;
                    for (int r = (int)(0.86 * fr->height); r < fr->height; r++)
                        for (int c = 0; c < fr->width; c++)
                            if (fr->data[0][r*fr->linesize[0]+c] > 150) b++;
                    bar_bright = b;
                }
                av_frame_unref(fr);
            }
    }
    /* flush decoder */
    avcodec_send_packet(dec, NULL);
    while (avcodec_receive_frame(dec, fr) == 0) { decoded++; av_frame_unref(fr); }

    printf("\ncaptured+encoded %d frames (%zu bytes, avg %zu/frame)\n",
           got, total, got ? total / got : 0);
    printf("decoded back %d frames\n", decoded);
    int overlay_ok = bar_bright > 40;   /* icon pixels survived encode/decode */
    printf("overlay bar: %d bright icon px -> %s\n", bar_bright,
           overlay_ok ? "BAR PRESENT" : "not detected");

    av_frame_free(&fr); av_packet_free(&pkt); avcodec_free_context(&dec);
    bsdr_overlay_free(ov);
    bsdr_capture_close(cap);

    int ok = got >= frames / 2 && decoded >= frames / 2 && overlay_ok;
    printf(ok ? "\nOK - desktop capture + NVENC encode + decode verified\n"
              : "\nFAILED\n");
    return ok ? 0 : 1;
}
