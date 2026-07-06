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
int bsdr_pw_capture_read(bsdr_pw_capture *c, uint8_t *d, int stride, int rows) {
    (void)c; (void)d; (void)stride; (void)rows; return -1;
}
void bsdr_pw_capture_close(bsdr_pw_capture *c) { (void)c; }

#else /* BSDR_HAVE_PIPEWIRE ==================================================================== */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <dbus/dbus.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>

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

    /* latest-frame slot (guarded) */
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint8_t        *frame;      /* packed 32-bit RGB, height*stride */
    int             stride;
    size_t          frame_cap;
    int             have_new;
    int             fatal;
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

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    bsdr_pw_capture *c = (bsdr_pw_capture *)data;
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_video_info info; memset(&info, 0, sizeof info);
    uint32_t mtype, msub;
    if (spa_format_parse(param, &mtype, &msub) < 0 ||
        mtype != SPA_MEDIA_TYPE_video || msub != SPA_MEDIA_SUBTYPE_raw) return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;
    pthread_mutex_lock(&c->lock);
    c->width  = info.info.raw.size.width;
    c->height = info.info.raw.size.height;
    c->fmt    = spa_to_fmt(info.info.raw.format);
    c->have_format = 1;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
    BSDR_INFO("bsdr.pw", "stream format: %dx%d fmt=%d", c->width, c->height, (int)c->fmt);
}

static void on_process(void *data) {
    bsdr_pw_capture *c = (bsdr_pw_capture *)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas > 0 && buf->datas[0].data && c->width > 0 && c->height > 0) {
        struct spa_data *d0 = &buf->datas[0];
        int src_stride = d0->chunk->stride > 0 ? d0->chunk->stride : c->width * 4;
        int rows = c->height;
        size_t need = (size_t)src_stride * rows;
        pthread_mutex_lock(&c->lock);
        if (c->frame_cap < need) {
            uint8_t *nb = realloc(c->frame, need);
            if (nb) { c->frame = nb; c->frame_cap = need; }
        }
        if (c->frame && c->frame_cap >= need) {
            memcpy(c->frame, d0->data, need);
            c->stride = src_stride;
            c->have_new = 1;
            pthread_cond_broadcast(&c->cond);
        }
        pthread_mutex_unlock(&c->lock);
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

bsdr_pw_capture *bsdr_pw_capture_open(int want_window, int cursor, int *ow, int *oh, bsdr_pw_format *ofmt) {
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
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);

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

    uint8_t podbuf[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(podbuf, sizeof podbuf);
    const struct spa_pod *params[1] = { build_format(&pb, w, h) };
    int rc = pw_stream_connect(c->stream, PW_DIRECTION_INPUT, node_id,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               params, 1);
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

int bsdr_pw_capture_read(bsdr_pw_capture *c, uint8_t *dst, int dst_stride, int dst_rows) {
    if (!c || !dst || dst_stride <= 0 || dst_rows <= 0) return -1;
    pthread_mutex_lock(&c->lock);
    if (c->fatal) { pthread_mutex_unlock(&c->lock); return -1; }
    if (!c->have_new) {                       /* wait up to ~100 ms for the next frame */
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&c->cond, &c->lock, &ts);
    }
    int ret = 0;
    if (c->have_new && c->frame) {
        int rows = c->height < dst_rows ? c->height : dst_rows;   /* clamp to both heights */
        int copy = c->stride < dst_stride ? c->stride : dst_stride;
        for (int y = 0; y < rows; y++)
            memcpy(dst + (size_t)y * dst_stride, c->frame + (size_t)y * c->stride, (size_t)copy);
        c->have_new = 0;
        ret = 1;
    }
    pthread_mutex_unlock(&c->lock);
    return ret;
}

void bsdr_pw_capture_close(bsdr_pw_capture *c) {
    if (!c) return;
    if (c->loop) pw_thread_loop_lock(c->loop);
    if (c->stream) { pw_stream_destroy(c->stream); c->stream = NULL; }
    if (c->core)   { pw_core_disconnect(c->core); c->core = NULL; }
    if (c->context){ pw_context_destroy(c->context); c->context = NULL; }
    if (c->loop)   { pw_thread_loop_unlock(c->loop); pw_thread_loop_stop(c->loop);
                     pw_thread_loop_destroy(c->loop); c->loop = NULL; }
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    free(c->frame);
    free(c);
}

#endif /* BSDR_HAVE_PIPEWIRE */
