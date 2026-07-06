/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* deps.h — optional 3rd-party dependency manager for the web control panel.
 *
 * Some features need an external program/driver we don't (or by license can't) ship inside bsdrX:
 * Npcap and WinDivert on Windows, VB-CABLE / BlackHole virtual-audio drivers, ViGEmBus, etc. This
 * module lets the web UI show the operator, per platform, which optional deps a feature needs, whether
 * each is already present, and — where the license permits AND we can automate it — install it on
 * request. Where we can't (proprietary EULA, or a kernel driver that must be user-approved), the UI
 * instead links to a per-dependency instructions page with the official download link.
 *
 * Detection is conservative: `present` is set only when we can positively confirm the dep (e.g. the
 * runtime DLL loads); we never falsely claim it's installed.
 */
#ifndef BSDR_DEPS_H
#define BSDR_DEPS_H

typedef struct {
    const char *id;         /* stable slug, e.g. "windivert" — used in /api URLs */
    const char *name;       /* display name, e.g. "WinDivert" */
    const char *purpose;    /* what bsdrX feature needs it */
    const char *license;    /* short license note */
    const char *info_url;   /* official download / info page */
    int present;            /* 1 = detected available on this machine */
    int bundled;            /* 1 = ships inside bsdrX (nothing to install) */
    int automatable;        /* 1 = bsdr_dep_install can install/launch it on request */
} bsdr_dep;

/* Fill up to `max` deps relevant to THIS platform, computing `present` for each. Returns the count. */
int bsdr_deps_list(bsdr_dep *out, int max);

/* Attempt to satisfy dependency `id` on request from the web UI. Writes a human-readable result into
 * `msg`. Returns 1 if it is (now) present / an installer was launched, 0 if the dep is manual-only
 * (caller should point the user at the instructions page), or -1 on error / unknown id. */
int bsdr_dep_install(const char *id, char *msg, int msgcap);

/* Return a static HTML instructions page (full <h..>/<p>/<a> fragment, no <html> wrapper) for `id`,
 * with the steps and the official download link, or NULL if `id` is unknown. */
const char *bsdr_dep_page(const char *id);

#endif /* BSDR_DEPS_H */
