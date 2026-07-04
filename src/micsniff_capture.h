/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Owner-mic sniffer — packet-capture backend (internal to micsniff).
 *
 * One small abstraction over two capture engines so the same sniffer runs everywhere:
 *   - libpcap (BSDR_HAVE_PCAP): macOS (BPF /dev/bpf), Windows (Npcap), and Linux when
 *     libpcap is present. This is the portable path.
 *   - AF_PACKET (Linux only): the zero-dependency fallback used when libpcap is absent.
 *
 * mc_cap_next() always returns a bare IPv4 packet (link-layer header stripped), so the
 * decoder above it (handle_ip) is fully platform-independent. mc_cap_inject() sends a raw
 * Ethernet frame (for ARP MITM) via whichever engine is active. This runs in the privileged
 * helper only — opening capture/inject needs root (CAP_NET_RAW / BPF access / Npcap). */
#ifndef BSDR_MICSNIFF_CAPTURE_H
#define BSDR_MICSNIFF_CAPTURE_H

#include <stddef.h>

typedef struct mc_cap mc_cap;

/* Open a promiscuous capture on `iface`, filtered to UDP from `quest_ip`. On failure returns
 * NULL and writes a message into err. */
mc_cap *mc_cap_open(const char *iface, const char *quest_ip, char *err, size_t errlen);

/* Next captured IPv4 packet (link header stripped) into buf. Returns byte count (>0), 0 on
 * timeout/no-IP, or -1 on a fatal error. Blocks up to timeout_ms. */
int mc_cap_next(mc_cap *c, unsigned char *buf, int maxlen, int timeout_ms);

/* Send a raw Ethernet frame (used for ARP). Returns 0 on success. */
int mc_cap_inject(mc_cap *c, const unsigned char *frame, int len);

/* "libpcap" or "af_packet", for logging. */
const char *mc_cap_backend(const mc_cap *c);

void mc_cap_close(mc_cap *c);

#endif /* BSDR_MICSNIFF_CAPTURE_H */
