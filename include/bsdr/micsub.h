/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* micsub.h — owner-mic SUBSTITUTION into the cloud (MITM only, Linux/NFQUEUE).
 *
 * The voice CHANGER (voicefx via micsniff) only affects the copy we expose locally; the Quest's
 * ORIGINAL voice still reaches the cloud room untouched. To make the ROOM hear the changed voice we
 * must rewrite the packets in flight. When we are the man-in-the-middle (ARP-spoofed router), the
 * Quest->cloud owner-mic packets transit our FORWARD chain, so we divert them to NFQUEUE and rewrite
 * the Opus payload IN PLACE with the voice-changed audio — keeping the RTP seq/timestamp/ssrc and the
 * 8-byte trailer, so the SFU sees a seamless continuation. The original is never re-sent.
 *
 * Requires CAP_NET_ADMIN (root): it installs an iptables NFQUEUE rule and binds the queue. Linux-only
 * (NFQUEUE); a no-op stub elsewhere or without libnetfilter_queue (BSDR_HAVE_NFQUEUE).
 */
#ifndef BSDR_MICSUB_H
#define BSDR_MICSUB_H

typedef struct bsdr_micsub bsdr_micsub;

/* Start substitution for the owner mic of `quest_ip`. `queue_num` is the NFQUEUE number to use.
 * Returns NULL if unavailable (not Linux, no libnetfilter_queue, no privileges, bad iptables). */
bsdr_micsub *bsdr_micsub_start(const char *quest_ip, int queue_num);

/* Live-update the voice change applied to the substituted audio (same knobs as voicefx). */
void bsdr_micsub_set_voicefx(bsdr_micsub *s, int gender, int robot, int echo, int whisper);

/* Remove the iptables rule and stop the queue thread. */
void bsdr_micsub_stop(bsdr_micsub *s);

#endif /* BSDR_MICSUB_H */
