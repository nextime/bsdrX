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
/* LAN discovery responder: listen for the headset's 5-byte magic broadcast on
 * UDP 45000 and reply on 45001 with `header + JSON` identity (per server.js
 * getDiscoverabilityData). */
#ifndef BSDR_DISCOVERY_H
#define BSDR_DISCOVERY_H

#include "bsdr/platform.h"
#include <stddef.h>

typedef struct {
    char session_id[129];
    char version[32];
    char device_name[128];
    char device_id[64];
    char pairing_request_code[8];  /* 6 numeric digits */
} bsdr_discovery_info;

/* Build the discovery response buffer (header + JSON). Returns its length. */
size_t bsdr_discovery_build(const bsdr_discovery_info *info,
                            uint8_t *out, size_t outlen);

typedef struct bsdr_discovery bsdr_discovery;

/* Start a background responder thread. `info` is copied. */
bsdr_discovery *bsdr_discovery_start(const bsdr_discovery_info *info);
void bsdr_discovery_stop(bsdr_discovery *d);

/* Called with each discovering Quest's IP (for the multi-Quest registry). */
void bsdr_discovery_set_on_seen(bsdr_discovery *d,
                                void (*cb)(const char *ip, void *user), void *user);

#endif /* BSDR_DISCOVERY_H */
