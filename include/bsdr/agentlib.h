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
/* Agent as a library: the whole agent wiring (discovery + control + transport +
 * capture/audio/input threads + web UI) behind one blocking call, so embedders
 * — the CLI main() and the Android JNI bridge — share the same lifecycle.
 *
 * agent.c's main() is a thin wrapper that parses argv into bsdr_agent_options
 * and calls bsdr_agent_run(); the Android app calls bsdr_agent_run() directly. */
#ifndef BSDR_AGENTLIB_H
#define BSDR_AGENTLIB_H

#include <stdbool.h>

/* Fired once, on bring-up, with the discovery pairing code (NUL-terminated).
 * Lets an embedder (e.g. the Android UI) show the code; the headset reads it
 * from the discovery reply automatically, so this is informational. */
typedef void (*bsdr_pairing_cb)(const char *code, void *user);

typedef struct {
    bool initiator;          /* true = DTLS client; default false (PC is server) */
    bool video, audio;       /* stream desktop video / audio both ways */
    bool control_only;       /* serve discovery+control only, no session worker */
    bool lan_live;           /* --lan: live desktop over the BigSoup LAN wire format */
    const char *video_file;  /* --file: stream this H.264/container instead of the desktop */
    bool file_gpu;           /* --file-gpu: encode the file on NVENC (default: libx264, better at low bitrate) */
    const char *replay_file; /* --replay: replay captured 45002 frames verbatim (no DTLS) */
    const char *quest_ip;    /* pair only with this headset (NULL = any) */
    int fps, bitrate;        /* 0 = defaults (30 fps, 8 Mbps) */
    int max_bitrate;         /* --max-bitrate: hard cap in bps, overrides the Quest; 0 = no cap */
    int screen_w, screen_h;  /* abs-pointer mapping; 0 = 1920x1080 */
    int dtls_timeout_ms;     /* 0 = 10000 */
    const char *cloud_data;       /* --cloud-data: "raw"|"dtls"|NULL(auto: raw then dtls) */
    const char *cloud_dtls_role;  /* --cloud-dtls-role: "client"|"server"|NULL(auto) */
    int cloud_latch_burst;        /* --cloud-latch-burst: # comedia keepalives on share start (default 12; 0=off) */
    int cloud_src_port;           /* --cloud-src-port: fixed local UDP source port for cloud video (0=ephemeral) */
    bool cloud_sticky_ports;      /* --cloud-sticky-ports: ephemeral 1st, then reuse same ports per relay IP across toggles */
    bool no_cloud_video;          /* --no-cloud-video: disable relay video (default: ON, trailer frags) */
    bool video_decoupled;         /* --video-decoupled: relay self-captures (default: couple to LAN encode) */
    bool cpu_only;                /* --cpu: force CPU scale/convert (default: try CUDA GPU pipeline) */
    bool lan_1x;                  /* --lan-1x: send LAN video once, not 2x (halves uplink on weak WiFi) */
    bool use_vaapi;               /* --vaapi: encode on the iGPU via VAAPI (frees the dGPU) */
    bool use_kmsgrab;             /* --kmsgrab: DRM/KMS capture (zero-copy with --vaapi; needs CAP_SYS_ADMIN) */
    bool use_sendmmsg;            /* --sendmmsg: batch LAN video fragments into one syscall */
    bool no_cloud_audio;          /* --no-cloud-audio: disable relay audio (default: ON, Opus + 8B trailer) */
    bool sniff_mic;               /* --sniff-mic: sniff the Quest's room mic -> BSDR-Quest-OwnerMic */
    bool sniff_mitm;              /* --sniff-mitm: ARP-spoof so a switched LAN routes it through us */
    const char *sniff_iface;      /* --sniff-iface: capture interface (NULL = default route) */
    const char *sniff_gw;         /* --sniff-gw: gateway IP for MITM (NULL = default route) */
    int  sniff_remote_port;       /* --sniff-remote: receive mic packets from the router companion
                                   * (bsdr_micrelay) on this UDP port; no local capture/MITM/root */
    bool compctl;                 /* --compctl: arm voice computer control (needs the owner mic + an LLM) */
    bool compctl_vision;          /* --compctl-vision: offer the model an on-demand desktop screenshot tool */
    int  listen_max_sec;          /* --listen-max: listening ceiling in seconds (0 = default 300 = 5 min) */
    int  confirm_sec;             /* --confirm-timeout: Send/Cancel auto-cancel in seconds (0 = default 60) */
    int  threed_mode;             /* --threed: 2D->3D SBS (0 off / 1 fast / 2 ai); forces CPU scale */
    int  threed_deepness;         /* --threed-deepness: depth amount 0..100 (0 = keep default 35) */
    int  threed_convergence;      /* --threed-convergence: screen-plane bias -50..50 */
    int  threed_swap;             /* --threed-swap: swap L/R eyes */
    int  threed_full;             /* --threed-full: full resolution per eye (default: light half-SBS) */
    const char *threed_ai_cmd;    /* --threed-ai: external depth-estimator command (AI mode) */
    int webui_port;          /* local control UI port; 0 = no UI */
    bool open_browser;       /* auto-open the control UI (desktop only) */
    bsdr_pairing_cb on_pairing_code;
    void *user;
} bsdr_agent_options;

/* Fill `o` with the agent's defaults (matches the CLI's no-flag behavior). */
void bsdr_agent_options_default(bsdr_agent_options *o);

/* Bring the agent up and pump until bsdr_agent_stop() (or SIGINT in the CLI)
 * is called. Blocks the calling thread. Returns 0 on clean shutdown. */
int bsdr_agent_run(const bsdr_agent_options *opt);

/* Ask a running bsdr_agent_run() to return (thread-safe; may be called from a
 * signal handler or another thread, e.g. the JNI stop path). */
void bsdr_agent_stop(void);

#endif /* BSDR_AGENTLIB_H */
