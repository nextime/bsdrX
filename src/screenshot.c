/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* One-shot desktop screenshot -> JPEG (X11 grab + libavcodec MJPEG on the desktop;
 * a MediaProjection + ImageReader grab in Kotlin on Android). */
#include "bsdr/screenshot.h"
#include "bsdr/log.h"

#if defined(__ANDROID__)

/* Android: the desktop is the device screen; capture goes through the same
 * MediaProjection Kotlin already holds. Route to the JNI bridge, which grabs one
 * frame and JPEG-encodes it (Bitmap.compress). */
#include "bsdr_android.h"
int bsdr_screenshot_jpeg(int max_dim, uint8_t *out, size_t cap) {
    return bsdr_android_screenshot(max_dim <= 0 ? 1280 : max_dim, out, cap);
}

#elif defined(__linux__) && defined(BSDR_ENABLE_VIDEO)

#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

int bsdr_screenshot_jpeg(int max_dim, uint8_t *out, size_t cap) {
    if (max_dim <= 0) max_dim = 1280;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { BSDR_WARN("bsdr.shot", "no X display"); return 0; }
    Window root = DefaultRootWindow(dpy);
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, root, &wa)) { XCloseDisplay(dpy); return 0; }
    int sw = wa.width, sh = wa.height;
    XImage *img = XGetImage(dpy, root, 0, 0, (unsigned)sw, (unsigned)sh, AllPlanes, ZPixmap);
    if (!img) { BSDR_WARN("bsdr.shot", "XGetImage failed"); XCloseDisplay(dpy); return 0; }

    /* Target dims: fit inside max_dim, keep aspect, even (JPEG 4:2:0). */
    int ow = sw, oh = sh;
    if (sw > max_dim || sh > max_dim) {
        double s = sw >= sh ? (double)max_dim / sw : (double)max_dim / sh;
        ow = (int)(sw * s); oh = (int)(sh * s);
    }
    ow &= ~1; oh &= ~1; if (ow < 2) ow = 2; if (oh < 2) oh = 2;

    int jn = 0;
    struct SwsContext *sws = NULL;
    AVFrame *yuv = NULL; AVPacket *pkt = NULL;
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *ctx = enc ? avcodec_alloc_context3(enc) : NULL;
    if (!ctx) goto done;
    ctx->width = ow; ctx->height = oh;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;             /* full-range JPEG */
    ctx->time_base = (AVRational){1, 25};
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * 6;         /* ~good quality, modest size */
    if (avcodec_open2(ctx, enc, NULL) < 0) goto done;

    /* XImage is packed 32-bit; on a normal little-endian display that's BGRA/BGR0. */
    sws = sws_getContext(sw, sh, AV_PIX_FMT_BGR0, ow, oh, AV_PIX_FMT_YUVJ420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    yuv = av_frame_alloc();
    if (!sws || !yuv) goto done;
    yuv->format = AV_PIX_FMT_YUVJ420P; yuv->width = ow; yuv->height = oh;
    if (av_frame_get_buffer(yuv, 32) < 0) goto done;
    {
        const uint8_t *src[4] = { (const uint8_t *)img->data, NULL, NULL, NULL };
        int srcstride[4] = { img->bytes_per_line, 0, 0, 0 };
        sws_scale(sws, src, srcstride, 0, sh, yuv->data, yuv->linesize);
    }
    yuv->quality = ctx->global_quality;
    yuv->pts = 0;
    pkt = av_packet_alloc();
    if (!pkt) goto done;
    if (avcodec_send_frame(ctx, yuv) == 0 && avcodec_receive_packet(ctx, pkt) == 0) {
        if ((size_t)pkt->size <= cap) { memcpy(out, pkt->data, (size_t)pkt->size); jn = pkt->size; }
        else BSDR_WARN("bsdr.shot", "jpeg %d > cap %zu", pkt->size, cap);
    }
done:
    if (pkt) av_packet_free(&pkt);
    if (yuv) av_frame_free(&yuv);
    if (sws) sws_freeContext(sws);
    if (ctx) avcodec_free_context(&ctx);
    XDestroyImage(img);
    XCloseDisplay(dpy);
    if (jn) BSDR_INFO("bsdr.shot", "desktop %dx%d -> jpeg %dx%d %d bytes", sw, sh, ow, oh, jn);
    return jn;
}

#else   /* no X11/video build */

int bsdr_screenshot_jpeg(int max_dim, uint8_t *out, size_t cap) {
    (void)max_dim; (void)out; (void)cap;
    return 0;
}

#endif
