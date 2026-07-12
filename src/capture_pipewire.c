/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* capture_pipewire.c — Wayland desktop capture via xdg-desktop-portal + PipeWire. See the header.
 *
 * Flow: connect the session bus -> ScreenCast.CreateSession -> SelectSources(monitor|window) ->
 * Start (returns the PipeWire node id + size) -> OpenPipeWireRemote (returns a PipeWire fd). Then a
 * pw_thread_loop drives a pw_stream on that fd/node; each delivered buffer (packed 32-bit RGB) is
 * copied into a latest-frame slot the puller reads. Portal calls are async "Request" objects: the
 * method reply carries a Request object path, and the real result arrives later as a Response signal
 * on that path — so we send, learn the path, then pump the bus until the matching Response lands. */
#include "bsdr/capture_pipewire.h"
#include "bsdr/log.h"

#ifndef BSDR_HAVE_PIPEWIRE
/* ---- stub build (no libpipewire/dbus): Wayland portal capture unavailable ------------------- */
int bsdr_pw_capture_available(void) { return 0; }
bsdr_pw_capture *bsdr_pw_capture_open(int w, int c, int *ow, int *oh, bsdr_pw_format *f) {
    (void)w; (void)c; (void)ow; (void)oh; (void)f; return (bsdr_pw_capture *)0;
}
int bsdr_pw_capture_read(bsdr_pw_capture *c, const uint8_t **f, int *stride, int *w, int *h) {
    (void)c; (void)f; (void)stride; (void)w; (void)h; return -1;
}
bsdr_pw_capture *bsdr_pw_capture_open2(int ww, int cur, int wd, int *ow, int *oh, bsdr_pw_format *f) {
    (void)ww; (void)cur; (void)wd; (void)ow; (void)oh; (void)f; return (bsdr_pw_capture *)0;
}
int bsdr_pw_capture_dmabuf_active(bsdr_pw_capture *c) { (void)c; return 0; }
void *bsdr_pw_capture_drm_frames_ctx(bsdr_pw_capture *c) { (void)c; return (void *)0; }
void *bsdr_pw_capture_read_drm(bsdr_pw_capture *c) { (void)c; return (void *)0; }
void bsdr_pw_capture_close(bsdr_pw_capture *c) { (void)c; }

#else /* BSDR_HAVE_PIPEWIRE ==================================================================== */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <dbus/dbus.h>

/* PipeWire's SPA header macros (the SPA_POD_*_INIT compound literals + spa_pod_*_init inline helpers)
 * deliberately leave the trailing `_padding` field uninitialised, which trips
 * -Wmissing-field-initializers all through this file under -Wextra. It's third-party-header noise, not
 * our bug and not fixable from here, so silence just that one warning for this PipeWire-only TU. The
 * pragma sits before the SPA includes so it also covers their inline-function bodies. */
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
/* enum spa_param_buffers (SPA_PARAM_BUFFERS_*): PipeWire >= 0.3.2x splits it into
 * <spa/param/buffers.h>; older releases (e.g. 0.3.19 on Debian 11) define it in
 * <spa/param/param.h>. Pick whichever this SPA provides. */
#if defined(__has_include) && __has_include(<spa/param/buffers.h>)
#  include <spa/param/buffers.h>
#else
#  include <spa/param/param.h>
#endif
#include <spa/pod/builder.h>

/* These SPA_POD_PROP_FLAG_* bits (used by the --pw-dmabuf modifier negotiation) were added to
 * spa/pod/pod.h after PipeWire 0.3.19 (Debian 11 ships 0.3.19, which has only READONLY/HARDWARE/
 * HINT_DICT). They're #define macros, so #ifndef cleanly detects their absence; define the standard,
 * stable bit values as a fallback so the dmabuf path builds on older SPA too. */
#ifndef SPA_POD_PROP_FLAG_MANDATORY
#  define SPA_POD_PROP_FLAG_MANDATORY   (1u << 3)
#endif
#ifndef SPA_POD_PROP_FLAG_DONT_FIXATE
#  define SPA_POD_PROP_FLAG_DONT_FIXATE (1u << 4)
#endif

/* EXPERIMENTAL dmabuf zero-copy path (--pw-dmabuf): negotiate a DRM dmabuf from PipeWire and hand
 * capture.c a DRM_PRIME AVFrame it can hwmap straight into VAAPI. Needs the libav hwcontext headers;
 * gated so a build without them still gets the CPU MAP_BUFFERS path. */
#if defined(BSDR_HAVE_CAPTURE)
#  include <libavutil/frame.h>
#  include <libavutil/buffer.h>
#  include <libavutil/hwcontext.h>
#  include <libavutil/hwcontext_drm.h>
#  include <libavutil/pixfmt.h>
#  define BSDR_PW_DMABUF 1
/* DRM fourcc / modifier constants (defined inline to avoid a libdrm-dev build dependency; these are
 * stable ABI values). fourcc('X','R','2','4') etc. */
#  define BSDR_DRM_FOURCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
#  define BSDR_DRM_FORMAT_XRGB8888 BSDR_DRM_FOURCC('X','R','2','4')
#  define BSDR_DRM_FORMAT_ARGB8888 BSDR_DRM_FOURCC('A','R','2','4')
#  define BSDR_DRM_FORMAT_XBGR8888 BSDR_DRM_FOURCC('X','B','2','4')
#  define BSDR_DRM_FORMAT_ABGR8888 BSDR_DRM_FOURCC('A','B','2','4')
#  define BSDR_DRM_MOD_INVALID     0x00ffffffffffffffULL
#endif

int bsdr_pw_capture_available(void) { return 1; }

struct bsdr_pw_capture {
    /* PipeWire */
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct pw_stream      *stream;
    struct spa_hook        stream_listener;
    int                    pw_fd;

    /* negotiated video */
    int            width, height;
    bsdr_pw_format fmt;
    int            have_format;

    /* Triple-buffer frame handoff. The producer (on_process) owns slots[widx] and writes it WITHOUT
     * the lock; the consumer (read) owns slots[ridx] and reads it WITHOUT the lock. {widx,rdyidx,ridx}
     * is always a permutation of {0,1,2}, so producer and consumer slots are always distinct — the
     * lock is taken only for the O(1) index swaps (publish / take), never during the ~8MB memcpy or
     * the downstream scale. This removes the second full-frame copy the puller used to do. */
    struct pw_slot { uint8_t *buf; size_t cap; int stride, w, h; } slots[3];
    int             widx, rdyidx, ridx;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             have_new;   /* a fresh frame is published in slots[rdyidx] */
    int             fatal;

#ifdef BSDR_PW_DMABUF
    /* --pw-dmabuf: negotiated dmabuf state. dmabuf_active flips on once on_param_changed sees a
     * modifier; until then (and on any failure) the CPU MAP_BUFFERS path above stays in use. */
    int             want_dmabuf;
    int             dmabuf_active;
    uint64_t        modifier;
    uint32_t        spa_fmt;        /* negotiated SPA_VIDEO_FORMAT_* */
    AVBufferRef    *drm_dev;        /* AV_HWDEVICE_TYPE_DRM (render node) */
    AVBufferRef    *drm_frames;     /* hw_frames_ctx wrapping the imported dmabufs (DRM_PRIME) */
    struct pw_buffer *db_pending;   /* newest un-taken dmabuf buffer (latest-wins; NULL if none) */
#endif
};

/* ============================================================================
 *  xdg-desktop-portal ScreenCast (libdbus)
 * ========================================================================== */
#define PORTAL_BUS   "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_SC    "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQ   "org.freedesktop.portal.Request"

static unsigned g_token_seq;   /* only touched from the single opening thread */

/* Append an `a{sv}` options dict opener; caller adds entries then closes with oa_close(). */
static void oa_open(DBusMessageIter *it, DBusMessageIter *dict) {
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", dict);
}
static void oa_close(DBusMessageIter *it, DBusMessageIter *dict) {
    dbus_message_iter_close_container(it, dict);
}
static void oa_str(DBusMessageIter *dict, const char *key, const char *val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}
static void oa_uint(DBusMessageIter *dict, const char *key, uint32_t val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "u", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}
static void oa_bool(DBusMessageIter *dict, const char *key, int val) {
    DBusMessageIter e, v; dbus_bool_t b = val ? TRUE : FALSE;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}
static void fresh_token(char *out, size_t cap) {
    snprintf(out, cap, "bsdr%u", ++g_token_seq);
}

/* Send a portal Request method and block until its Response signal arrives. Returns the Response
 * message (caller must unref) with `*resp` set to the portal response code (0 = success), plus its
 * iterator positioned at the trailing results `a{sv}`. NULL on error/timeout. */
static DBusMessage *portal_request(DBusConnection *conn, DBusMessage *call, uint32_t *resp,
                                   DBusMessageIter *results_out) {
    DBusError err; dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 8000, &err);
    if (!reply) { BSDR_WARN("bsdr.pw", "portal call failed: %s", err.message ? err.message : "?");
                  dbus_error_free(&err); return NULL; }
    const char *handle = NULL;
    if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID) || !handle) {
        BSDR_WARN("bsdr.pw", "portal reply had no request handle"); dbus_message_unref(reply);
        dbus_error_free(&err); return NULL;
    }
    char rule[256];
    snprintf(rule, sizeof rule,
             "type='signal',interface='" PORTAL_REQ "',member='Response',path='%s'", handle);
    dbus_bus_add_match(conn, rule, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);

    /* pump the bus for up to ~30 s waiting for the Response on this request path */
    DBusMessage *out = NULL;
    for (int spins = 0; spins < 3000 && !out; spins++) {
        if (!dbus_connection_read_write_dispatch(conn, 10)) break;   /* disconnected */
        DBusMessage *m;
        while ((m = dbus_connection_pop_message(conn))) {
            if (dbus_message_is_signal(m, PORTAL_REQ, "Response") &&
                dbus_message_has_path(m, handle)) {
                out = m; break;
            }
            dbus_message_unref(m);
        }
    }
    dbus_bus_remove_match(conn, rule, NULL);
    if (!out) { BSDR_WARN("bsdr.pw", "portal: no Response signal (timeout/cancel)"); return NULL; }
    /* Response(u response, a{sv} results) */
    DBusMessageIter it;
    if (!dbus_message_iter_init(out, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
        dbus_message_unref(out); return NULL;
    }
    dbus_message_iter_get_basic(&it, resp);
    dbus_message_iter_next(&it);                 /* now at results a{sv} */
    if (results_out) *results_out = it;
    return out;
}

/* Find a string/objpath value for `key` in a results a{sv} iterator (copied into out). */
static int results_get_str(DBusMessageIter results, const char *key, char *out, size_t cap) {
    DBusMessageIter arr;
    if (dbus_message_iter_get_arg_type(&results) != DBUS_TYPE_ARRAY) return -1;
    dbus_message_iter_recurse(&results, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e; dbus_message_iter_recurse(&arr, &e);
        const char *k = NULL; dbus_message_iter_get_basic(&e, &k);
        dbus_message_iter_next(&e);
        if (k && strcmp(k, key) == 0) {
            DBusMessageIter v; dbus_message_iter_recurse(&e, &v);
            int t = dbus_message_iter_get_arg_type(&v);
            if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) {
                const char *s = NULL; dbus_message_iter_get_basic(&v, &s);
                if (s) { snprintf(out, cap, "%s", s); return 0; }
            }
        }
        dbus_message_iter_next(&arr);
    }
    return -1;
}

/* Parse the Start results `streams` = a(ua{sv}); take the first stream's node id (and size). */
static int results_get_stream(DBusMessageIter results, uint32_t *node_id, int *w, int *h) {
    DBusMessageIter arr;
    if (dbus_message_iter_get_arg_type(&results) != DBUS_TYPE_ARRAY) return -1;
    dbus_message_iter_recurse(&results, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e; dbus_message_iter_recurse(&arr, &e);
        const char *k = NULL; dbus_message_iter_get_basic(&e, &k);
        dbus_message_iter_next(&e);
        if (k && strcmp(k, "streams") == 0) {
            DBusMessageIter v; dbus_message_iter_recurse(&e, &v);           /* variant -> a(ua{sv}) */
            if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_ARRAY) return -1;
            DBusMessageIter sarr; dbus_message_iter_recurse(&v, &sarr);
            if (dbus_message_iter_get_arg_type(&sarr) != DBUS_TYPE_STRUCT) return -1;
            DBusMessageIter st; dbus_message_iter_recurse(&sarr, &st);      /* (u a{sv}) */
            dbus_message_iter_get_basic(&st, node_id);
            *w = *h = 0;
            dbus_message_iter_next(&st);                                    /* props a{sv} */
            if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY) {
                DBusMessageIter parr; dbus_message_iter_recurse(&st, &parr);
                while (dbus_message_iter_get_arg_type(&parr) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter pe; dbus_message_iter_recurse(&parr, &pe);
                    const char *pk = NULL; dbus_message_iter_get_basic(&pe, &pk);
                    dbus_message_iter_next(&pe);
                    if (pk && strcmp(pk, "size") == 0) {                    /* variant (ii) */
                        DBusMessageIter pv; dbus_message_iter_recurse(&pe, &pv);
                        if (dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_STRUCT) {
                            DBusMessageIter si; dbus_message_iter_recurse(&pv, &si);
                            dbus_message_iter_get_basic(&si, w); dbus_message_iter_next(&si);
                            dbus_message_iter_get_basic(&si, h);
                        }
                    }
                    dbus_message_iter_next(&parr);
                }
            }
            return 0;
        }
        dbus_message_iter_next(&arr);
    }
    return -1;
}

static DBusMessage *sc_method(const char *method) {
    return dbus_message_new_method_call(PORTAL_BUS, PORTAL_PATH, PORTAL_SC, method);
}

/* Run CreateSession/SelectSources/Start and OpenPipeWireRemote. Fills session/node/size + returns
 * the PipeWire fd (>=0) or -1. */
static int portal_screencast(DBusConnection *conn, int want_window, int cursor,
                             uint32_t *node_id, int *w, int *h) {
    char tok[32], stok[64], session[256] = "";
    uint32_t resp = 1;
    DBusMessage *r, *call;
    DBusMessageIter it, results;

    /* 1) CreateSession */
    call = sc_method("CreateSession");
    dbus_message_iter_init_append(call, &it);
    { DBusMessageIter d; oa_open(&it, &d);
      fresh_token(tok, sizeof tok); oa_str(&d, "handle_token", tok);
      snprintf(stok, sizeof stok, "bsdrsess%u", g_token_seq); oa_str(&d, "session_handle_token", stok);
      oa_close(&it, &d); }
    r = portal_request(conn, call, &resp, &results); dbus_message_unref(call);
    if (!r) return -1;
    if (resp != 0 || results_get_str(results, "session_handle", session, sizeof session) != 0) {
        BSDR_WARN("bsdr.pw", "CreateSession failed (resp=%u)", resp); dbus_message_unref(r); return -1;
    }
    dbus_message_unref(r);

    /* 2) SelectSources(session, {types, cursor_mode, multiple}) */
    call = sc_method("SelectSources");
    dbus_message_iter_init_append(call, &it);
    { const char *sp = session; dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &sp); }
    { DBusMessageIter d; oa_open(&it, &d);
      fresh_token(tok, sizeof tok); oa_str(&d, "handle_token", tok);
      oa_uint(&d, "types", want_window ? 2u : 1u);       /* 1=MONITOR, 2=WINDOW */
      oa_uint(&d, "cursor_mode", cursor ? 2u : 1u);      /* 1=hidden, 2=embedded */
      oa_bool(&d, "multiple", 0);
      oa_close(&it, &d); }
    r = portal_request(conn, call, &resp, &results); dbus_message_unref(call);
    if (!r) return -1;
    if (resp != 0) { BSDR_WARN("bsdr.pw", "SelectSources failed (resp=%u)", resp); dbus_message_unref(r); return -1; }
    dbus_message_unref(r);

    /* 3) Start(session, parent_window="", {}) -> streams */
    call = sc_method("Start");
    dbus_message_iter_init_append(call, &it);
    { const char *sp = session, *parent = ""; dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &sp);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent); }
    { DBusMessageIter d; oa_open(&it, &d);
      fresh_token(tok, sizeof tok); oa_str(&d, "handle_token", tok);
      oa_close(&it, &d); }
    r = portal_request(conn, call, &resp, &results); dbus_message_unref(call);
    if (!r) return -1;
    if (resp != 0 || results_get_stream(results, node_id, w, h) != 0) {
        BSDR_WARN("bsdr.pw", "Start failed / no stream (resp=%u)", resp); dbus_message_unref(r); return -1;
    }
    dbus_message_unref(r);
    BSDR_INFO("bsdr.pw", "portal screencast: node %u size %dx%d", *node_id, *w, *h);

    /* 4) OpenPipeWireRemote(session, {}) -> fd (plain reply, not a Request) */
    call = sc_method("OpenPipeWireRemote");
    dbus_message_iter_init_append(call, &it);
    { const char *sp = session; dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &sp); }
    { DBusMessageIter d; oa_open(&it, &d); oa_close(&it, &d); }
    DBusError err; dbus_error_init(&err);
    r = dbus_connection_send_with_reply_and_block(conn, call, 8000, &err);
    dbus_message_unref(call);
    if (!r) { BSDR_WARN("bsdr.pw", "OpenPipeWireRemote failed: %s", err.message ? err.message : "?");
              dbus_error_free(&err); return -1; }
    int fd = -1;
    if (!dbus_message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
        BSDR_WARN("bsdr.pw", "OpenPipeWireRemote: no fd"); dbus_message_unref(r); dbus_error_free(&err); return -1;
    }
    dbus_message_unref(r);
    return fd;
}

/* ============================================================================
 *  PipeWire stream
 * ========================================================================== */
static bsdr_pw_format spa_to_fmt(enum spa_video_format f) {
    switch (f) {
        case SPA_VIDEO_FORMAT_BGRx: return BSDR_PW_FMT_BGR0;
        case SPA_VIDEO_FORMAT_RGBx: return BSDR_PW_FMT_RGB0;
        case SPA_VIDEO_FORMAT_BGRA: return BSDR_PW_FMT_BGRA;
        case SPA_VIDEO_FORMAT_RGBA: return BSDR_PW_FMT_RGBA;
        default: return BSDR_PW_FMT_BGR0;
    }
}

#ifdef BSDR_PW_DMABUF
static const struct spa_pod *build_format_dmabuf(struct spa_pod_builder *, int, int,
                                                 const uint64_t *, int, int);
static uint32_t spa_fmt_to_drm(uint32_t, enum AVPixelFormat *);
static int pw_dmabuf_make_frames(bsdr_pw_capture *, int, int, enum AVPixelFormat);
#endif

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    bsdr_pw_capture *c = (bsdr_pw_capture *)data;
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_video_info info; memset(&info, 0, sizeof info);
    uint32_t mtype, msub;
    if (spa_format_parse(param, &mtype, &msub) < 0 ||
        mtype != SPA_MEDIA_TYPE_video || msub != SPA_MEDIA_SUBTYPE_raw) return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

#ifdef BSDR_PW_DMABUF
    if (c->want_dmabuf) {
        const struct spa_pod_prop *mp = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
        if (mp) {
            /* dmabuf variant was chosen. If the modifier is still a choice (DONT_FIXATE), re-send a
             * fixed EnumFormat to collapse it, then wait for the fixed param_changed. */
            if (SPA_POD_CHOICE_TYPE(&mp->value) != SPA_CHOICE_None) {
                uint64_t chosen = info.info.raw.modifier;
                uint8_t bf[1024]; struct spa_pod_builder b = SPA_POD_BUILDER_INIT(bf, sizeof bf);
                const struct spa_pod *p[1] = { build_format_dmabuf(&b,
                    info.info.raw.size.width, info.info.raw.size.height, &chosen, 1, 1) };
                pw_stream_update_params(c->stream, p, 1);
                return;   /* fixed format arrives as a second param_changed */
            }
            /* fixed modifier: set up the DRM_PRIME frames ctx + advertise dmabuf buffers */
            enum AVPixelFormat sw = AV_PIX_FMT_BGR0;
            uint32_t fourcc = spa_fmt_to_drm(info.info.raw.format, &sw);
            if (fourcc && pw_dmabuf_make_frames(c, info.info.raw.size.width,
                                                info.info.raw.size.height, sw) == 0) {
                c->modifier = info.info.raw.modifier;
                c->spa_fmt  = info.info.raw.format;
                c->dmabuf_active = 1;
                int32_t stride = (int)info.info.raw.size.width * 4;
                uint8_t bb[512]; struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(bb, sizeof bb);
                const struct spa_pod *bp[1] = { spa_pod_builder_add_object(&b2,
                    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                    SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(6, 3, 8),
                    SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
                    SPA_PARAM_BUFFERS_size,     SPA_POD_Int(stride * (int)info.info.raw.size.height),
                    SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(stride),
                    SPA_PARAM_BUFFERS_align,    SPA_POD_Int(16),
                    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_DmaBuf)) };
                pw_stream_update_params(c->stream, bp, 1);
                BSDR_INFO("bsdr.pw", "dmabuf negotiated: %ux%u fmt=%u mod=0x%llx",
                          info.info.raw.size.width, info.info.raw.size.height,
                          c->spa_fmt, (unsigned long long)c->modifier);
            } else {
                BSDR_WARN("bsdr.pw", "dmabuf import setup failed -> CPU path");
                c->dmabuf_active = 0;
            }
        } else {
            c->dmabuf_active = 0;   /* server picked the SHM fallback variant */
        }
    }
#endif

    pthread_mutex_lock(&c->lock);
    c->width  = info.info.raw.size.width;
    c->height = info.info.raw.size.height;
    c->fmt    = spa_to_fmt(info.info.raw.format);
    c->have_format = 1;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
    int db = 0;
#ifdef BSDR_PW_DMABUF
    db = c->dmabuf_active;
#endif
    BSDR_INFO("bsdr.pw", "stream format: %dx%d fmt=%d%s", c->width, c->height, (int)c->fmt,
              db ? " (dmabuf)" : "");
}

static void on_process(void *data) {
    bsdr_pw_capture *c = (bsdr_pw_capture *)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;

#ifdef BSDR_PW_DMABUF
    if (buf->n_datas > 0 && buf->datas[0].type == SPA_DATA_DmaBuf) {
        if (!c->dmabuf_active) { pw_stream_queue_buffer(c->stream, b); return; }  /* not ready yet */
        /* Hold this buffer (latest-wins): stash it as the pending frame and requeue any previous
         * un-taken pending. The buffer handed to the consumer is only requeued from the AVFrame's
         * free callback (read_drm), never here — the compositor mustn't reuse a surface VAAPI is
         * still reading. */
        pthread_mutex_lock(&c->lock);
        struct pw_buffer *old = c->db_pending;
        c->db_pending = b;
        c->have_new = 1;
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
        if (old) pw_stream_queue_buffer(c->stream, old);
        return;
    }
#endif

    if (buf->n_datas > 0 && buf->datas[0].data && c->width > 0 && c->height > 0) {
        struct spa_data *d0 = &buf->datas[0];
        int src_stride = d0->chunk->stride > 0 ? d0->chunk->stride : c->width * 4;
        int rows = c->height;
        size_t need = (size_t)src_stride * rows;
        /* We exclusively own slots[widx] — resize + fill it WITHOUT the lock (the consumer can never
         * be on this slot), then take the lock only to publish it. */
        struct pw_slot *s = &c->slots[c->widx];
        if (s->cap < need) {
            uint8_t *nb = realloc(s->buf, need);
            if (nb) { s->buf = nb; s->cap = need; }
        }
        if (s->buf && s->cap >= need) {
            memcpy(s->buf, d0->data, need);
            s->stride = src_stride; s->w = c->width; s->h = rows;
            pthread_mutex_lock(&c->lock);
            int t = c->widx; c->widx = c->rdyidx; c->rdyidx = t;   /* publish: widx <-> rdyidx */
            c->have_new = 1;
            pthread_cond_broadcast(&c->cond);
            pthread_mutex_unlock(&c->lock);
        }
    }
    pw_stream_queue_buffer(c->stream, b);
}

static const struct pw_stream_events STREAM_EVENTS = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
};

/* Build the EnumFormat param the stream offers (BGRx/RGBx/BGRA/RGBA, any size, up to 240 fps). */
static const struct spa_pod *build_format(struct spa_pod_builder *b, int w, int h) {
    return spa_pod_builder_add_object(b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE((uint32_t)(w > 0 ? w : 1920), (uint32_t)(h > 0 ? h : 1080)),
            &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(240, 1)));
}

#ifdef BSDR_PW_DMABUF
/* Build a dmabuf-capable EnumFormat: same as build_format but with a hand-built modifier property
 * (MANDATORY | DONT_FIXATE) so the server re-fixates onto one modifier. `mods`/`n_mod` is the list we
 * accept; we always include DRM_FORMAT_MOD_INVALID (implicit modifier — the widest-compatible option,
 * what most compositors offer). If `fixate` we emit a single fixed modifier instead of the choice
 * (used from on_param_changed to collapse the negotiation). */
static const struct spa_pod *build_format_dmabuf(struct spa_pod_builder *b, int w, int h,
                                                 const uint64_t *mods, int n_mod, int fixate) {
    struct spa_pod_frame f0;
    spa_pod_builder_push_object(b, &f0, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
        SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
        SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA), 0);
    /* the modifier property, with the flags the convenience macros can't express */
    if (fixate) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(b, mods[0]);
    } else {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                             SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        struct spa_pod_frame fc;
        spa_pod_builder_push_choice(b, &fc, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(b, mods[0]);           /* default (repeated as first alternative) */
        for (int i = 0; i < n_mod; i++) spa_pod_builder_long(b, mods[i]);
        spa_pod_builder_pop(b, &fc);
    }
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
        &SPA_RECTANGLE((uint32_t)(w > 0 ? w : 1920), (uint32_t)(h > 0 ? h : 1080)),
        &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
        &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(240, 1)), 0);
    return spa_pod_builder_pop(b, &f0);
}

/* SPA format -> DRM fourcc (SPA names bytes, DRM names LE packed words: BGRx->xRGB, etc.) plus the
 * matching ffmpeg sw pixel format. Returns 0 on an unhandled format. */
static uint32_t spa_fmt_to_drm(uint32_t spa_fmt, enum AVPixelFormat *sw) {
    switch (spa_fmt) {
        case SPA_VIDEO_FORMAT_BGRx: if (sw) *sw = AV_PIX_FMT_BGR0; return BSDR_DRM_FORMAT_XRGB8888;
        case SPA_VIDEO_FORMAT_BGRA: if (sw) *sw = AV_PIX_FMT_BGRA; return BSDR_DRM_FORMAT_ARGB8888;
        case SPA_VIDEO_FORMAT_RGBx: if (sw) *sw = AV_PIX_FMT_RGB0; return BSDR_DRM_FORMAT_XBGR8888;
        case SPA_VIDEO_FORMAT_RGBA: if (sw) *sw = AV_PIX_FMT_RGBA; return BSDR_DRM_FORMAT_ABGR8888;
        default: return 0;
    }
}

/* (Re)build the DRM_PRIME hw_frames_ctx for the negotiated geometry/format. 0 ok, -1 fail. */
static int pw_dmabuf_make_frames(bsdr_pw_capture *c, int w, int h, enum AVPixelFormat sw) {
    if (!c->drm_dev) return -1;
    if (c->drm_frames) av_buffer_unref(&c->drm_frames);
    c->drm_frames = av_hwframe_ctx_alloc(c->drm_dev);
    if (!c->drm_frames) return -1;
    AVHWFramesContext *fc = (AVHWFramesContext *)c->drm_frames->data;
    fc->format    = AV_PIX_FMT_DRM_PRIME;
    fc->sw_format = sw;
    fc->width     = w;
    fc->height    = h;
    if (av_hwframe_ctx_init(c->drm_frames) < 0) { av_buffer_unref(&c->drm_frames); return -1; }
    return 0;
}

/* AVFrame free callback: release the held PipeWire buffer back to the pool. Runs on the consumer
 * thread once the filtergraph is done with the surface; takes the pw loop lock (pw_stream_* is not
 * thread-safe). `data` is the embedded descriptor, freed with the holder. */
struct db_hold { bsdr_pw_capture *c; struct pw_buffer *b; AVDRMFrameDescriptor desc; };
static void db_frame_free(void *opaque, uint8_t *data) {
    struct db_hold *h = (struct db_hold *)opaque; (void)data;
    if (h->c->loop && h->c->stream) {
        pw_thread_loop_lock(h->c->loop);
        pw_stream_queue_buffer(h->c->stream, h->b);
        pw_thread_loop_unlock(h->c->loop);
    }
    free(h);
}

static void db_requeue(bsdr_pw_capture *c, struct pw_buffer *b) {
    pw_thread_loop_lock(c->loop);
    pw_stream_queue_buffer(c->stream, b);
    pw_thread_loop_unlock(c->loop);
}

int bsdr_pw_capture_dmabuf_active(bsdr_pw_capture *c) { return c ? c->dmabuf_active : 0; }
void *bsdr_pw_capture_drm_frames_ctx(bsdr_pw_capture *c) {
    return (c && c->dmabuf_active) ? (void *)c->drm_frames : NULL;
}

void *bsdr_pw_capture_read_drm(bsdr_pw_capture *c) {
    if (!c || !c->dmabuf_active) return NULL;
    pthread_mutex_lock(&c->lock);
    if (c->fatal) { pthread_mutex_unlock(&c->lock); return NULL; }
    if (!c->have_new) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&c->cond, &c->lock, &ts);
    }
    struct pw_buffer *b = NULL;
    if (c->have_new && c->db_pending) { b = c->db_pending; c->db_pending = NULL; c->have_new = 0; }
    uint32_t fw = c->width, fh = c->height, sfmt = c->spa_fmt; uint64_t mod = c->modifier;
    AVBufferRef *frames = c->drm_frames;
    pthread_mutex_unlock(&c->lock);
    if (!b) return NULL;

    struct spa_buffer *buf = b->buffer;
    struct spa_data *d = buf->datas;
    enum AVPixelFormat sw;
    uint32_t fourcc = spa_fmt_to_drm(sfmt, &sw);
    int n = buf->n_datas; if (n > AV_DRM_MAX_PLANES) n = AV_DRM_MAX_PLANES;
    struct db_hold *h = calloc(1, sizeof *h);
    AVFrame *f = av_frame_alloc();
    if (!fourcc || !h || !f) { if (f) av_frame_free(&f); free(h); db_requeue(c, b); return NULL; }
    h->c = c; h->b = b;
    h->desc.nb_objects = n;
    h->desc.nb_layers  = 1;
    h->desc.layers[0].format    = fourcc;
    h->desc.layers[0].nb_planes = n;
    for (int i = 0; i < n; i++) {
        h->desc.objects[i].fd              = (int)d[i].fd;
        h->desc.objects[i].size            = 0;
        h->desc.objects[i].format_modifier = mod;
        h->desc.layers[0].planes[i].object_index = i;
        h->desc.layers[0].planes[i].offset = d[i].chunk ? (ptrdiff_t)d[i].chunk->offset : 0;
        h->desc.layers[0].planes[i].pitch  = d[i].chunk ? (ptrdiff_t)d[i].chunk->stride : 0;
    }
    f->format = AV_PIX_FMT_DRM_PRIME;
    f->width  = (int)fw; f->height = (int)fh;
    f->data[0] = (uint8_t *)&h->desc;
    f->buf[0]  = av_buffer_create((uint8_t *)&h->desc, sizeof h->desc,
                                  db_frame_free, h, AV_BUFFER_FLAG_READONLY);
    if (!f->buf[0]) { av_frame_free(&f); free(h); db_requeue(c, b); return NULL; }
    if (frames) f->hw_frames_ctx = av_buffer_ref(frames);
    return f;
}
#endif /* BSDR_PW_DMABUF */

bsdr_pw_capture *bsdr_pw_capture_open(int want_window, int cursor, int *ow, int *oh, bsdr_pw_format *ofmt) {
    return bsdr_pw_capture_open2(want_window, cursor, 0, ow, oh, ofmt);
}

bsdr_pw_capture *bsdr_pw_capture_open2(int want_window, int cursor, int want_dmabuf,
                                       int *ow, int *oh, bsdr_pw_format *ofmt) {
    /* portal handshake first (blocking dialog) — this establishes the fd + node id */
    DBusError err; dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) { BSDR_WARN("bsdr.pw", "no session bus: %s", err.message ? err.message : "?");
                 dbus_error_free(&err); return NULL; }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    uint32_t node_id = 0; int w = 0, h = 0;
    int fd = portal_screencast(conn, want_window, cursor, &node_id, &w, &h);
    if (fd < 0) return NULL;   /* conn is a shared ref; don't unref */

    bsdr_pw_capture *c = calloc(1, sizeof *c);
    if (!c) { return NULL; }
    c->pw_fd = fd; c->width = w; c->height = h; c->fmt = BSDR_PW_FMT_BGR0;
    c->widx = 0; c->rdyidx = 1; c->ridx = 2;   /* distinct slots (calloc would alias them at 0) */
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);

#ifdef BSDR_PW_DMABUF
    /* Only offer dmabuf if we can actually create the DRM import device — otherwise we'd negotiate
     * dmabuf buffers we can't map. Failure here silently keeps the CPU MAP_BUFFERS path. */
    if (want_dmabuf) {
        if (av_hwdevice_ctx_create(&c->drm_dev, AV_HWDEVICE_TYPE_DRM, "/dev/dri/renderD128", NULL, 0) == 0)
            c->want_dmabuf = 1;
        else
            BSDR_WARN("bsdr.pw", "--pw-dmabuf: no DRM device on /dev/dri/renderD128 -> CPU path");
    }
#else
    (void)want_dmabuf;
#endif

    pw_init(NULL, NULL);
    c->loop = pw_thread_loop_new("bsdr-pw", NULL);
    if (!c->loop) { BSDR_WARN("bsdr.pw", "pw_thread_loop_new failed"); goto fail; }
    c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (!c->context) { BSDR_WARN("bsdr.pw", "pw_context_new failed"); goto fail; }
    pw_thread_loop_lock(c->loop);
    if (pw_thread_loop_start(c->loop) != 0) { pw_thread_loop_unlock(c->loop); BSDR_WARN("bsdr.pw", "loop start failed"); goto fail; }
    c->core = pw_context_connect_fd(c->context, fd, NULL, 0);   /* takes ownership of fd */
    if (!c->core) { pw_thread_loop_unlock(c->loop); BSDR_WARN("bsdr.pw", "connect_fd failed"); goto fail; }
    c->stream = pw_stream_new(c->core, "bsdr-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", NULL));
    if (!c->stream) { pw_thread_loop_unlock(c->loop); BSDR_WARN("bsdr.pw", "stream_new failed"); goto fail; }
    pw_stream_add_listener(c->stream, &c->stream_listener, &STREAM_EVENTS, c);

    uint8_t podbuf[2048];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(podbuf, sizeof podbuf);
    const struct spa_pod *params[2]; int nparams = 0;
#ifdef BSDR_PW_DMABUF
    /* dmabuf variant first (preferred); the plain SHM format stays as the fallback the server picks
     * if it can't do dmabuf. DRM_FORMAT_MOD_INVALID = implicit modifier (widest compatibility). */
    if (c->want_dmabuf) {
        static const uint64_t mods[1] = { BSDR_DRM_MOD_INVALID };
        params[nparams++] = build_format_dmabuf(&pb, w, h, mods, 1, 0);
    }
#endif
    params[nparams++] = build_format(&pb, w, h);
    int rc = pw_stream_connect(c->stream, PW_DIRECTION_INPUT, node_id,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               params, nparams);
    pw_thread_loop_unlock(c->loop);
    if (rc != 0) { BSDR_WARN("bsdr.pw", "stream_connect failed (%d)", rc); goto fail; }

    /* wait (briefly) for format negotiation so we can report the real size/format */
    pthread_mutex_lock(&c->lock);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (!c->have_format && !c->fatal) if (pthread_cond_timedwait(&c->cond, &c->lock, &ts) != 0) break;
    int gw = c->width, gh = c->height; bsdr_pw_format gf = c->fmt;
    pthread_mutex_unlock(&c->lock);
    if (gw <= 0 || gh <= 0) { gw = w; gh = h; }
    if (ow) *ow = gw;
    if (oh) *oh = gh;
    if (ofmt) *ofmt = gf;
    BSDR_INFO("bsdr.pw", "Wayland capture ready: %dx%d", gw, gh);
    return c;
fail:
    bsdr_pw_capture_close(c);
    return NULL;
}

int bsdr_pw_capture_read(bsdr_pw_capture *c, const uint8_t **out_frame,
                         int *out_stride, int *out_w, int *out_h) {
    if (!c || !out_frame) return -1;
    pthread_mutex_lock(&c->lock);
    if (c->fatal) { pthread_mutex_unlock(&c->lock); return -1; }
    if (!c->have_new) {                       /* wait up to ~100 ms for the next frame */
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&c->cond, &c->lock, &ts);
    }
    int ret = 0;
    if (c->have_new) {
        int t = c->ridx; c->ridx = c->rdyidx; c->rdyidx = t;   /* take: ridx <-> rdyidx */
        c->have_new = 0;
        ret = 1;
    }
    struct pw_slot *s = &c->slots[c->ridx];
    pthread_mutex_unlock(&c->lock);
    if (ret && s->buf) {
        *out_frame = s->buf;                  /* borrowed until the next read() call */
        if (out_stride) *out_stride = s->stride;
        if (out_w) *out_w = s->w;
        if (out_h) *out_h = s->h;
        return 1;
    }
    return 0;
}

void bsdr_pw_capture_close(bsdr_pw_capture *c) {
    if (!c) return;
    /* NOTE: any AVFrame handed out by bsdr_pw_capture_read_drm MUST be released before close — its
     * free callback touches c->stream. bsdr_capture_frame unrefs each frame within the same call, so
     * none is outstanding here. */
    if (c->loop) pw_thread_loop_lock(c->loop);
#ifdef BSDR_PW_DMABUF
    if (c->db_pending && c->stream) { pw_stream_queue_buffer(c->stream, c->db_pending); c->db_pending = NULL; }
#endif
    if (c->stream) { pw_stream_destroy(c->stream); c->stream = NULL; }
    if (c->core)   { pw_core_disconnect(c->core); c->core = NULL; }
    if (c->context){ pw_context_destroy(c->context); c->context = NULL; }
    if (c->loop)   { pw_thread_loop_unlock(c->loop); pw_thread_loop_stop(c->loop);
                     pw_thread_loop_destroy(c->loop); c->loop = NULL; }
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    for (int i = 0; i < 3; i++) free(c->slots[i].buf);
#ifdef BSDR_PW_DMABUF
    if (c->drm_frames) av_buffer_unref(&c->drm_frames);
    if (c->drm_dev)    av_buffer_unref(&c->drm_dev);
#endif
    free(c);
}

#endif /* BSDR_HAVE_PIPEWIRE */
