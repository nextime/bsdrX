/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* screenblank_wayland.c — privacy screen-blank on wlroots Wayland via wlr-gamma-control.
 *
 * Wayland has no core gamma access, but wlroots compositors (sway, Hyprland, wayfire, river...) expose
 * zwlr_gamma_control_v1: bind it per output and hand the compositor a black gamma ramp (all-zero LUT),
 * so the physical output goes dark while the portal/PipeWire capture keeps reading intact frames — the
 * same principle as X11 RandR brightness. The compositor RESTORES the original gamma when the client
 * disconnects, so we must hold the connection for the whole blank: a worker thread keeps it alive until
 * unblank. Built only with BSDR_HAVE_WAYLAND_GAMMA (libwayland-client + the scanner-generated glue).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1          /* memfd_create */
#endif
#include "bsdr/log.h"
#include "bsdr/platform.h"

#include <wayland-client.h>
#include "wlr-gamma-control-client-protocol.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>

#define WLB_MAXOUT 16

static struct {
    struct wl_display *dpy;
    struct zwlr_gamma_control_manager_v1 *mgr;
    struct wl_output *outputs[WLB_MAXOUT];
    struct zwlr_gamma_control_v1 *gc[WLB_MAXOUT];
    int    nout;
    bsdr_thread *thr;
    volatile int stop;
    int    running;
} W;

/* Fill a black gamma ramp (3 * size uint16 zeros) into an anonymous fd and hand it to the compositor. */
static void set_black_gamma(struct zwlr_gamma_control_v1 *gc, uint32_t size) {
    size_t bytes = (size_t)size * 3 * sizeof(uint16_t);
    int fd = memfd_create("bsdr-gamma", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return; }   /* zero-filled == black ramp */
    zwlr_gamma_control_v1_set_gamma(gc, fd);
    close(fd);
}

static void gc_gamma_size(void *data, struct zwlr_gamma_control_v1 *gc, uint32_t size) {
    (void)data;
    if (size > 0) set_black_gamma(gc, size);
}
static void gc_failed(void *data, struct zwlr_gamma_control_v1 *gc) {
    (void)data; (void)gc;
    BSDR_WARN("bsdr.blank", "wlr gamma control failed for an output (another client holds it?)");
}
static const struct zwlr_gamma_control_v1_listener GC_L = { gc_gamma_size, gc_failed };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)data; (void)ver;
    if (!strcmp(iface, zwlr_gamma_control_manager_v1_interface.name))
        W.mgr = wl_registry_bind(reg, name, &zwlr_gamma_control_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && W.nout < WLB_MAXOUT)
        W.outputs[W.nout++] = wl_registry_bind(reg, name, &wl_output_interface, 1);
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener REG_L = { reg_global, reg_remove };

/* Keep the connection alive (holding the black gamma) until asked to stop, then let it drop so the
 * compositor restores the original ramp. */
static void wl_keepalive(void *arg) {
    (void)arg;
    int fd = wl_display_get_fd(W.dpy);
    while (!W.stop) {
        wl_display_flush(W.dpy);
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&p, 1, 200);
        if (r > 0 && (p.revents & POLLIN)) { if (wl_display_dispatch(W.dpy) < 0) break; }
        else wl_display_dispatch_pending(W.dpy);
    }
    for (int i = 0; i < W.nout; i++) if (W.gc[i]) { zwlr_gamma_control_v1_destroy(W.gc[i]); W.gc[i] = NULL; }
    wl_display_flush(W.dpy);
    wl_display_disconnect(W.dpy);         /* disconnect -> compositor restores gamma */
    W.dpy = NULL;
}

/* Returns 0 if handled (wlroots gamma control present), -1 if unavailable (fall back / no-op). */
int bsdr_wl_gamma_blank(int on) {
    if (!on) {
        if (W.running) { W.stop = 1; if (W.thr) { bsdr_thread_join(W.thr); W.thr = NULL; } W.running = 0; }
        return 0;
    }
    if (W.running) return 0;                          /* already blank */
    memset(&W, 0, sizeof W);
    W.dpy = wl_display_connect(NULL);
    if (!W.dpy) return -1;
    struct wl_registry *reg = wl_display_get_registry(W.dpy);
    wl_registry_add_listener(reg, &REG_L, NULL);
    wl_display_roundtrip(W.dpy);                      /* collect manager + outputs */
    if (!W.mgr || W.nout == 0) { wl_display_disconnect(W.dpy); W.dpy = NULL; return -1; }
    for (int i = 0; i < W.nout; i++) {
        W.gc[i] = zwlr_gamma_control_manager_v1_get_gamma_control(W.mgr, W.outputs[i]);
        if (W.gc[i]) zwlr_gamma_control_v1_add_listener(W.gc[i], &GC_L, NULL);
    }
    wl_display_roundtrip(W.dpy);                      /* triggers gamma_size -> set black ramp */
    W.stop = 0;
    W.thr = bsdr_thread_start(wl_keepalive, NULL);
    if (!W.thr) { wl_display_disconnect(W.dpy); W.dpy = NULL; return -1; }
    W.running = 1;
    BSDR_INFO("bsdr.blank", "Wayland privacy screen-blank on (%d output(s) via wlr-gamma-control)", W.nout);
    return 0;
}
