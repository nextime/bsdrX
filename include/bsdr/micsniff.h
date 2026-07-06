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
/* Owner-mic sniffer (Linux only).
 *
 * The Bigscreen remote-desktop protocol has NO mic-upload channel: a real
 * Quest<->host capture (full.pcapng) shows the headset sends the host only
 * fixed-size controller/input telemetry on 45004 and video keepalives on 45002
 * — never a variable-bitrate Opus stream. The headset owner's voice instead
 * goes to the Bigscreen *room* (the mediasoup cloud SFU), as PLAIN, unencrypted
 * Opus RTP (proven from libBigSoup.so: RTP header + [opus][u32 ssrc LE]
 * [u32 frame_id LE], each packet sent twice).
 *
 * So to expose the owner's voice as a local microphone we intercept that
 * Quest->cloud RTP off the LAN, decode the Opus, and feed it into a dedicated
 * virtual mic "BSDR_QuestMic" — carrying the headset owner's voice.
 *
 * Capture needs the agent to actually see the Quest->cloud packets. On a flat
 * hub/SPAN/gateway setup, plain promiscuous capture suffices. On a switched LAN
 * (the common case), enable `mitm` to ARP-spoof the Quest<->gateway path so the
 * traffic transits this host (IP forwarding is toggled on and restored on stop).
 */
#ifndef BSDR_MICSNIFF_H
#define BSDR_MICSNIFF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct bsdr_micsniff bsdr_micsniff;

/* Optional tap on the decoded owner voice: interleaved int16 PCM at 48 kHz.
 * The sniffer feeds every decoded frame here (in addition to the virtual mic) so
 * the voice-command pipeline can transcribe the headset owner. `channels` is 1
 * (the owner mic is mono). Runs on the sniffer thread — keep it cheap. */
typedef void (*bsdr_micsniff_pcm_cb)(void *user, const int16_t *pcm,
                                     int frames, int channels);

typedef struct {
    const char *quest_ip;    /* REQUIRED: only decode RTP whose IPv4 source is this headset. */
    const char *iface;       /* capture interface; NULL = auto (the default-route interface). */
    const char *gateway_ip;  /* MITM target; NULL = auto (the default-route gateway). */
    bool        mitm;        /* ARP-spoof Quest<->gateway so their traffic passes through us. */
    const char *password;    /* sudo password for the privileged helper when not already root.
                              * NULL => interactive `sudo` (prompts on the terminal, for CLI use);
                              * non-NULL => `sudo -S` fed on stdin (for the web UI, which has no tty). */
    int         remote_port; /* >0: receive captured packets from the router companion (bsdr_micrelay)
                              * on this UDP port instead of capturing locally. No root/MITM needed;
                              * works over WiFi since the router legitimately sees the Quest's traffic. */
} bsdr_micsniff_cfg;

/* Privileged-helper entry point. The agent re-execs itself (via sudo) with `--sniff-helper`;
 * main() must route that to here. It opens the AF_PACKET capture socket (and, for MITM, the ARP
 * socket + /proc knobs), hands the capture fd back to the unprivileged parent over a Unix socket,
 * then either exits (passive) or stays as the root ARP process (MITM) until the parent detaches.
 * Returns a process exit code. Non-Linux builds return 1. */
/* The sniffer (micsniff.c) is only compiled in audio-enabled desktop builds. For
 * input-only configs (make linux-static, the plain `windows` target, any
 * --disable-audio build) AND on Android (no root -> no promiscuous capture, so
 * shared agent flow links; the owner mic is simply unavailable there. Android has no local packet
 * capture, but CAN receive the router-companion relay (remote_port) — so it gets the real API too. */
#if defined(BSDR_HAVE_AUDIO)

int bsdr_micsniff_helper_main(int argc, char **argv);

/* Start the sniffer (spawns its own thread). Creates the "BSDR_QuestMic"
 * virtual device and streams the decoded owner voice into it. Returns NULL if
 * unavailable (non-Linux build, missing CAP_NET_RAW, bad interface, no quest_ip). */
bsdr_micsniff *bsdr_micsniff_start(const bsdr_micsniff_cfg *cfg);

/* Stop + join the sniffer, tear down the virtual device, and (if MITM was on)
 * heal the victims' ARP caches and restore ip_forward. Safe on NULL. */
void bsdr_micsniff_stop(bsdr_micsniff *s);

/* True if the running sniffer is in MITM mode (so a mode change forces a restart). */
bool bsdr_micsniff_is_mitm(const bsdr_micsniff *s);

/* Install (or clear, cb=NULL) a PCM tap for the voice-command pipeline. Safe to
 * call on a live sniffer; safe on NULL. */
void bsdr_micsniff_set_pcm_sink(bsdr_micsniff *s, bsdr_micsniff_pcm_cb cb, void *user);

/* Realtime voice change applied to the decoded owner voice before it reaches the virtual mic / cloud
 * / command tap. gender -100..100 (pitch/formant), robot/echo/whisper 0..100. Safe on a live sniffer. */
void bsdr_micsniff_set_voicefx(bsdr_micsniff *s, int gender, int robot, int echo, int whisper);

/* Cloud voice SUBSTITUTION over the relay: when on (and running in router-companion mode), bsdrX
 * re-encodes the voice-changed owner audio and sends the modified RTP to the relay, which forwards it
 * to the cloud in place of the Quest's original (the room hears the changed voice). This is the relay
 * equivalent of the in-flight NFQUEUE/WinDivert substitution; available on every platform that can run
 * the relay. Safe on a live sniffer / NULL. */
void bsdr_micsniff_set_substitute(bsdr_micsniff *s, int on);

#else  /* no audio backend: inert stubs */

static inline int  bsdr_micsniff_helper_main(int argc, char **argv) { (void)argc; (void)argv; return 1; }
static inline bsdr_micsniff *bsdr_micsniff_start(const bsdr_micsniff_cfg *cfg) { (void)cfg; return (bsdr_micsniff *)0; }
static inline void bsdr_micsniff_stop(bsdr_micsniff *s) { (void)s; }
static inline bool bsdr_micsniff_is_mitm(const bsdr_micsniff *s) { (void)s; return false; }
static inline void bsdr_micsniff_set_pcm_sink(bsdr_micsniff *s, bsdr_micsniff_pcm_cb cb, void *user) { (void)s; (void)cb; (void)user; }
static inline void bsdr_micsniff_set_voicefx(bsdr_micsniff *s, int g, int r, int e, int w) { (void)s;(void)g;(void)r;(void)e;(void)w; }
static inline void bsdr_micsniff_set_substitute(bsdr_micsniff *s, int on) { (void)s; (void)on; }

#endif /* BSDR_HAVE_AUDIO */

#endif /* BSDR_MICSNIFF_H */
