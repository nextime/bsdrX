/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* bsdrX owner-mic RELAY control protocol — the wire contract shared by the router-side companion
 * (tools/bsdr_micrelay.c) and the agent-side sniffer (src/micsniff.c).
 *
 * Transport: one UDP channel per side.
 *   - The relay binds BSDR_RELAY_PORT (45099) and broadcasts a HELLO beacon so agents find it with
 *     no hand-typed IPs.
 *   - Each agent opens its own sniff socket (ephemeral src port); it unicasts REGISTER to the relay
 *     and the relay returns that headset's mic + ACKs to wherever the REGISTER came from. The src
 *     address of a REGISTER IS the flow's return path — no forward-port needs to be configured.
 *
 * One relay serves MANY agents in parallel: it keeps a flow table keyed by headset (Quest) IP, and
 * demuxes captured mic packets to each headset's registered agent. Authorisation is BIND-TO-OWNER:
 * the relay is in-path, so it observes each headset<->agent bsdrX session (UDP on the ports in
 * BSDR_RELAY_PAIR_PORTS) and only honours a REGISTER for headset H from the agent it actually saw
 * paired with H. A LAN host cannot siphon a headset it isn't running the remote-desktop session for.
 *
 * Framing. Two kinds of datagram travel on these sockets:
 *   1) CONTROL messages: 4-byte magic "BSRL", 1-byte version, 1-byte type, then a typed body.
 *   2) MIC DATA (relay -> agent only): a bare IPv4 packet (link header already stripped), forwarded
 *      verbatim. Its first byte is 0x45..0x4f (IPv4 version<<4 | ihl); control's first byte is 'B'
 *      (0x42), so the receiver tells them apart by byte 0 with no extra tag. */
#ifndef BSDR_RELAYPROTO_H
#define BSDR_RELAYPROTO_H

#include <stdint.h>
#include <string.h>

#define BSDR_RELAY_PORT     45099            /* relay control/discovery UDP port */
#define BSDR_RELAY_MAGIC0   'B'
#define BSDR_RELAY_MAGIC1   'S'
#define BSDR_RELAY_MAGIC2   'R'
#define BSDR_RELAY_MAGIC3   'L'
#define BSDR_RELAY_VERSION  1

/* Message types (byte 5). */
#define BSDR_RELAY_HELLO    'H'   /* relay<->agent discovery beacon: body = role byte */
#define BSDR_RELAY_REGISTER 'R'   /* agent->relay: capture headset X, forward its mic to me (heartbeat) */
#define BSDR_RELAY_UNREG    'U'   /* agent->relay: stop capturing headset X for me */
#define BSDR_RELAY_ACK      'K'   /* relay->agent: registration result */
/* Cloud voice SUBSTITUTION (agent->relay), each now tagged with the headset IP so the relay applies
 * it to the right flow. The relay never codecs: bsdrX ships the modified datagram, the relay injects. */
#define BSDR_RELAY_CTRL     'C'   /* substitute mode + cloud dst for headset X */
#define BSDR_RELAY_FULLDG   'F'   /* full modified IPv4 datagram (src=Quest) to raw-inject to the cloud */
#define BSDR_RELAY_RTP      'A'   /* legacy: modified RTP payload to forward to the cloud via UDP socket */

/* HELLO roles. */
#define BSDR_RELAY_ROLE_RELAY 'R'
#define BSDR_RELAY_ROLE_AGENT 'A'

/* Registration outcome (ACK status byte). */
#define BSDR_RELAY_OK          0
#define BSDR_RELAY_DENY_UNPAIRED 1   /* relay never observed this agent paired with that headset */

/* UDP ports that mark a live bsdrX LAN session between a headset and an agent — the relay watches
 * these to build the bind-to-owner map (discovery 45000, media 45002, data 45004). */
#define BSDR_RELAY_PAIR_PORTS { 45000, 45002, 45004 }

/* A control datagram is one that starts with the "BSRL" magic (vs a forwarded IPv4 mic packet). */
static inline int bsdr_relay_is_ctrl(const void *p, int len) {
    const unsigned char *b = (const unsigned char *)p;
    return len >= 6 && b[0] == BSDR_RELAY_MAGIC0 && b[1] == BSDR_RELAY_MAGIC1 &&
           b[2] == BSDR_RELAY_MAGIC2 && b[3] == BSDR_RELAY_MAGIC3;
}

/* Write the 6-byte header (magic+version+type). Returns bytes written (6). */
static inline int bsdr_relay_hdr(unsigned char *m, char type) {
    m[0] = BSDR_RELAY_MAGIC0; m[1] = BSDR_RELAY_MAGIC1; m[2] = BSDR_RELAY_MAGIC2; m[3] = BSDR_RELAY_MAGIC3;
    m[4] = BSDR_RELAY_VERSION; m[5] = (unsigned char)type;
    return 6;
}

#endif /* BSDR_RELAYPROTO_H */
