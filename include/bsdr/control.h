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
/* HTTP control server (port 45678) + pairing state machine.
 *
 * Mirrors build/app/server.js: a shared 6-digit pairingRequestCode (advertised
 * via discovery) gates POST /pair, which mints a long pairingId that gates
 * /heartbeat, /start, /device, /stop, /unpair. One paired device at a time.
 */
#ifndef BSDR_CONTROL_H
#define BSDR_CONTROL_H

#include "bsdr/platform.h"
#include <stdbool.h>
#include <time.h>

typedef struct {
    char pairing_id[97];      /* 48 bytes hex */
    char device_id[128];
    char device_name[128];
    char remote_ip[64];
    time_t created_at;
    time_t last_keepalive;
    bool is_sharing;
    /* last applied tunables (one per PUT /device, like the original) */
    long bitrate, fec, fps, resolution;
    int  internet_sharing;   /* this /device PUT's isInternetSharing: 1=on, 0=off, -1=absent */
} bsdr_paired_device;

/* Callbacks fire from the control thread; keep them non-blocking. */
typedef void (*bsdr_control_cb)(const bsdr_paired_device *dev, void *user);

typedef struct {
    bsdr_control_cb on_start;
    bsdr_control_cb on_stop;
    bsdr_control_cb on_unpair;
    bsdr_control_cb on_settings;
    /* Optional: gate /pair by source IP (multi-Quest selection). NULL => allow. */
    bool (*allow_pair)(const char *ip, void *user);
    void *user;
} bsdr_control_cbs;

typedef struct bsdr_control bsdr_control;

bsdr_control *bsdr_control_start(const char *pairing_code,
                                 const bsdr_control_cbs *cbs);
void bsdr_control_stop(bsdr_control *c);

/* Call periodically: forgets the device if silent past the heartbeat window.
 * Returns true if a device was just forgotten (so the caller can tear down). */
bool bsdr_control_expire_stale(bsdr_control *c);

/* Operator-initiated drop: forget the paired device so it must re-pair.
 * Returns true if a device was actually forgotten. Does not fire callbacks —
 * the caller owns teardown (mirrors bsdr_control_expire_stale). */
bool bsdr_control_force_unpair(bsdr_control *c);

/* CSPRNG helpers (OpenSSL-backed) used for ids/codes. */
void bsdr_gen_hex(char *out, size_t nbytes);     /* writes 2*nbytes hex + NUL */
void bsdr_gen_pairing_code(char *out6);          /* 6 digits + NUL */

#endif /* BSDR_CONTROL_H */
