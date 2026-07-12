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
/* Shared application state behind the web UI: cloud login, discovered Quests +
 * selection, pairing/streaming status, and the video source. Thread-safe. */
#ifndef BSDR_APP_H
#define BSDR_APP_H

#include "bsdr/platform.h"
#include <stdbool.h>
#include <stddef.h>

#define BSDR_MAX_QUESTS 16

typedef struct {
    char ip[64];
    char name[64];
    uint64_t last_seen_ms;
} bsdr_quest_entry;

typedef struct bsdr_app {
    bsdr_mutex *lock;
    /* cloud */
    bool cloud_logged_in;
    char cloud_email[128];
    char cloud_name[128];
    char cloud_msg[160];
    char access_token[2048];
    char refresh_token[2048];     /* for re-login at startup without a password */
    /* Second "bot" account — its own login + presence WS + room-join, independent of the host session
     * (no media). Used to satisfy the Quest owner-mic gate (Room.participants > 1) by sitting in the
     * host's room, and as the base for future in-room moderation. */
    bool bot_logged_in;
    char bot_email[128];
    char bot_name[128];
    char bot_msg[160];
    char bot_access_token[2048];
    char bot_refresh_token[2048];
    char bot_room_id[128];        /* the room the bot last joined (status only) */
    char bot_social_id[80];       /* the bot account's socialId (for the host to invite it) */
    bool bot_joined;
    bool bot_stopped;             /* user hit "Stop": WS/room presence down but the login is remembered */
    void *bot_ws;                 /* bsdr_cloud_ws* presence handle for the bot account */
    char bot_mode[8];             /* "audio" (REST join only, owner-mic unlock) | "full" (avatar) */
    void *bot_room;               /* bsdr_botroom* avatar presence handle (full mode); NULL otherwise */
    bool bot_follow;              /* follow-me: re-join whatever room the operator moves into */
    bool bot_loopback;            /* cloud-mic loopback: route the bot's room audio -> BSDR_RoomMic
                                   * (carries the operator's OWN voice; works solo) */
    bool bot_solo_owner;          /* "listen only to me": solo the room owner's SSRC in the loopback */
    volatile int bot_roommic_active; /* set while the bot owns BSDR_RoomMic (cloud_mic_main defers) */
    void *bot_audio;              /* bsdr_botaudio* loopback handle; NULL otherwise */
    char bot_audio_ip[64];        /* bot's room-audio relay + port + the room owner's session id, */
    int  bot_audio_port;          /* captured at join so the loopback toggle can (re)start without a */
    char bot_owner_sid[200];      /* re-join (owner sid drives the "listen only to me" solo) */
    /* cloud internet sharing (Mediasoup relay) */
    void *cloud_ws;               /* bsdr_cloud_ws* presence handle (host online) */
    void *cloud_stream;           /* bsdr_cloud_stream* active relay streaming */
    bool internet_sharing;        /* operator/Quest requested internet sharing */
    bool cloud_auto_share;        /* follow the Quest's RDC screen (auto start/stop) */
    int  cloud_screen_misses;     /* consecutive /rooms polls with no screen (debounce stop) */
    bool cloud_starting;          /* a stream-start is in flight (prevents a double-start race) */
    bool unpair_pending;          /* Quest unpaired (or heartbeat lost) — cloud teardown is on a grace timer */
    uint64_t unpair_deadline_ms;  /* when the grace expires and we actually stop the relay (0 = none) */
    char cloud_data_mode[8];      /* "" (auto) | "raw" | "dtls" — cloud data channel transport */
    char cloud_dtls_role[8];      /* "" (auto) | "client" | "server" — cloud data DTLS role */
    bool cloud_no_video;          /* --no-cloud-video: don't produce relay video (default: video ON) */
    bool video_decoupled;         /* --video-decoupled: relay runs its own capture+encoder (default:
                                   * coupled — relay the single LAN encode, half the capture/encode cost) */
    bool cpu_only;                /* --cpu: force CPU scale/convert (default: try CUDA GPU pipeline) */
    bool use_vaapi;               /* --vaapi: encode on the iGPU via VAAPI */
    bool use_kmsgrab;             /* --kmsgrab: DRM/KMS capture (zero-copy with --vaapi) */
    int  enc_level;               /* encoder effort: 0 quality (default) / 1 balanced / 2 performance */
    int  enc_x264_threads;        /* opt-in (P6.9): >1 = N x264 frame threads on the live --cpu path */
    bool lan_1x;                  /* send LAN video once instead of 2x — halves the WiFi uplink */
    int  fps_cap;                 /* cap the desktop capture/encode fps (0 = default/30) */
    bool wifi_opt;                /* Wi-Fi network optimization: DSCP/WMM-mark the LAN media sockets */
    bool force_x11;               /* --x11: force x11grab; never the Wayland portal */
    bool force_pipewire;          /* --wayland/--pipewire: force the portal + PipeWire capture */
    bool pw_dmabuf;               /* --pw-dmabuf (experimental): zero-copy PipeWire dmabuf -> VAAPI */
    bool cloud_no_audio;          /* relay audio: plain Opus RTP (pt 100, djb2 SSRC, ts+=480, no
                                   * trailer); default ON (false) -> --no-cloud-audio disables */
    /* discovered Quests + selection */
    bsdr_quest_entry quests[BSDR_MAX_QUESTS];
    int quest_count;
    char selected_quest_ip[64];   /* "" => accept any */
    char blocked_quest_ip[64];    /* disconnected by operator; refused until reselected */
    volatile unsigned select_gen; /* bumped when the operator picks a different headset */
    /* live connection */
    bool quest_paired;
    char quest_name[128];
    char quest_ip[64];
    bool streaming;
    bool paused;                  /* web stop/restart */
    bool disconnect_req;          /* operator asked to drop the paired Quest */
    /* source */
    char source[16];              /* "desktop" | "file" | "webcam" | "webcam3d" | "terminal" */
    char source_path[512];        /* file path/URL, (webcam) primary/left camera, or (terminal) the shell/command */
    char source_path2[512];       /* webcam3d: the right-eye camera device */
    /* Terminal source (headless console streaming): render a shell to the headset with the Quest's
     * keyboard/mouse injected. "pty" = in-process libvterm (no X needed); "xvfb" = a private Xvfb +
     * xterm captured via x11grab (XTEST injection). term_cols/rows size the pty grid (0 = default). */
    char term_backend[8];         /* "pty" | "xvfb" (empty = pty) */
    int  term_cols, term_rows;    /* pty grid size; 0 = default (120x36) */
    volatile unsigned source_gen; /* bump when the source changes so the live session reopens capture */
    volatile unsigned encoder_gen;/* bump on a CPU<->GPU encoder toggle so the session reopens capture
                                   * IN PLACE (no full restart, which would drop the input channel) */
    /* In-VR media-bar playback controls for file streaming — shared by the LAN video+audio threads,
     * the input thread, and the cloud audio sender. Edge-triggered seek via a generation counter. */
    volatile int file_paused;     /* play/pause */
    volatile int file_loop;       /* 1 = play the file/playlist continuously in loop (web UI / overlay) */
    volatile int file_volume;     /* 0..100 audio gain (init 100) */
    volatile unsigned file_seek_gen; /* bump to request a seek to file_seek_frac */
    double file_seek_frac;        /* seek target 0..1 (written before bumping file_seek_gen) */
    bool blank_want;              /* privacy: blank the physical monitor while the Quest is connected */
    bool pointer_touch;           /* input pointer mode: 0 = mouse (tap/drag as clicks), 1 = real touch */
    /* 2D->3D side-by-side: applied on the encode path (forces CPU scale). Read by the LAN streamer
     * when it (re)opens the capture; a change takes effect on the next capture reopen. */
    int threed_mode;              /* bsdr_threed_mode: 0 off / 1 fast / 2 ai */
    int threed_deepness;          /* 0..100 depth amount */
    int threed_convergence;       /* -50..50 screen-plane bias */
    int threed_swap;              /* swap L/R eyes */
    int threed_full;              /* 1 = full resolution per eye (2x-wide encode); default on */
    int threed_tier;              /* AI mode in-process depth tier: 0 external/none, 1 cpu, 2 gpu, 3 hi */
    char threed_ai_cmd[256];      /* external depth helper command (AI mode) */
    volatile unsigned threed_gen; /* bumped on any 3D change so the streamer reopens the capture */
    /* Owner-mic cloud fallback: when the LAN sniffer / router companion isn't available (e.g. WiFi),
     * use the cloud room audio as the compctl owner mic. A voice-activity duck isolates the owner
     * (loudest speaker) only while a command is captured. See cloud_stream.c / audio.c. */
    bool cloud_mic_fallback;
    bool owner_mic_local;         /* use THIS machine's microphone as the owner mic (no sniff/relay) */
    volatile int cloud_mic_duck;  /* armed by the agent while a voice command is being captured */
    void (*room_pcm_cb)(void *user, const int16_t *pcm, int frames, int channels);
    void *room_pcm_user;          /* set-user-before-cb; cloud thread reads without a lock */
    bool audio;
    /* capture region: a single window (x,y,w,h). all 0 = whole desktop. */
    int cap_x, cap_y, cap_w, cap_h;
    /* video quality (applied on next session / source change) */
    int res_w, res_h;             /* 0 = source resolution */
    int bitrate;                  /* bps — the EFFECTIVE encode bitrate (what the streamer reads) */
    int quest_bitrate;            /* bps — last value the headset asked for (PUT /device) */
    int bitrate_override;         /* web UI override: 0 = follow the headset, >0 = force this bitrate */
    int max_bitrate;              /* --max-bitrate: hard cap (bps); 0 = no cap (follow the Quest) */
    /* voice assistant config (STT + LLM endpoints) */
    char stt_endpoint[256], stt_token[256], stt_model[64];
    char llm_endpoint[256], llm_token[256], llm_model[64];

    /* computer control (voice -> STT -> LLM tools). Enableable only while the
     * owner-mic sniffer/MITM is active (that's the only source of the owner's
     * voice). Desired state driven by the web UI; reconciled in the main loop. */
    bool compctl_want;            /* operator asked for computer control */
    bool compctl_vision;          /* also send the desktop screenshot to the (vision) model */
    bool compctl_dirty;           /* one-shot: desired state changed */
    bool compctl_active;          /* status: voice pipeline currently armed */
    char compctl_msg[128];        /* status/error line for the web UI */

    int cloud_latch_burst;        /* --cloud-latch-burst: comedia keepalives on share start */
    int cloud_src_port;           /* --cloud-src-port: fixed local source port for cloud video */
    bool cloud_sticky_ports;      /* --cloud-sticky-ports: reuse ephemeral ports per relay across toggles */

    /* owner-mic sniffer (desired state driven by CLI + web UI; reconciled in the main loop) */
    bool sniff_want;              /* should the owner-mic sniffer be running */
    int  sniff_method;            /* 0 = passive sniff, 1 = MITM (ARP), 2 = router relay */
    bool sniff_mitm;              /* derived: method == 1 (kept for the reconcile path) */
    bool sniff_dirty;             /* one-shot: desired state changed, main loop should reconcile */
    char sniff_password[128];     /* transient sudo password from the web UI (cleared after use) */
    bool sniff_active;            /* status: sniffer currently running */
    char sniff_msg[128];          /* status/error line for the web UI */
    int  sniff_remote_port;       /* router-companion relay port (used when method == relay) */
    int  sniff_wifi;              /* cached: is the default capture NIC Wi-Fi? -1 unknown, 0 no, 1 yes
                                   * (drives the UI's MITM-over-Wi-Fi cancel/continue prompt) */
    bool room_mic_want;           /* expose the cloud room's voice mix as a virtual mic "BSDR_RoomMic" */
    /* realtime voice change on the Quest mic: master on/off, gender -100..100 (0 = off); and, in
     * MITM/relay mode, substitute = stop the Quest->cloud voice and inject the changed audio. */
    bool voice_fx_on;             /* master enable for the voice changer (sliders ignored when off) */
    int  voice_gender;    /* pitch/character (granular shift) */
    int  voice_formant;   /* tone/brightness tilt */
    int  voice_volume;    /* output gain */
    int  voice_robot;
    int  voice_echo;
    int  voice_whisper;
    bool voice_substitute;
    /* AI (RVC) voice-conversion tier — sits behind the same voice changer. When on, the mic is
     * converted to voiceai_voice; the DSP knobs above are bypassed (except volume). */
    bool voiceai_on;              /* use the AI tier instead of the DSP tier */
    int  voiceai_tier;            /* 1=cpu 2=small-gpu 3=big-gpu */
    char voiceai_voice[64];       /* target voice id (voicestore library) */
    int  voiceai_key;             /* pitch shift, semitones (-24..24; +12 = up an octave) */
    char voiceai_status[128];     /* engine status text for the web UI */
    volatile unsigned voiceai_gen; /* bumped on any voiceai config change (reconcile trigger) */
    /* up to 5 named custom voice presets (snapshot of the whole changer state) */
    struct bsdr_voice_preset {
        char name[40];            /* empty = unused slot */
        int  ai_on, tier, key;
        char voice[64];
        int  gender, formant, volume, robot, echo, whisper;
    } voice_presets[5];
    /* realtime face swap: enable + tier (0 auto/cpu,2 gpu,3 hi) + source image path; gen bumps on
     * change so the live session reopens the capture (face swap forces the CPU encode path). */
    bool faceswap_on;
    int  faceswap_tier;
    char faceswap_source[512];
    volatile unsigned faceswap_gen;
    char faceswap_status[128];
    int  faceswap_detect_every;   /* opt-in (P4.5): detect every N frames (>=2), swap every frame; 0/1 = every frame */
    /* Status-poll cache for faceswap model presence — avoids a stat/mkdir storm on every /api/status
     * (1 s while a tab is open). Refreshed on faceswap_gen bump, while a download is active, or ~2 s max. */
    char     fs_dir_cache[930];
    int      fs_dir_cached;
    int      fs_present_cache[4]; /* >= BSDR_FACESWAP_NFILES */
    int      fs_ready_cache;
    unsigned fs_present_gen;
    uint64_t fs_present_ms;
    int      fs_present_valid;
} bsdr_app;

void bsdr_app_init(bsdr_app *a);
void bsdr_app_free(bsdr_app *a);

/* Playlist support (stateless helpers; no bsdr_app needed). A source path ending in ".txt" is a
 * playlist: one video path per line, blank lines and #comments skipped, relative paths resolved
 * against the .txt's directory. Each streamer walks the list independently, advancing at EOF. */
bool bsdr_path_is_playlist(const char *src);
/* True if `s` starts with an allowlisted streamable URL scheme (http://, https://, rtsp://). */
bool bsdr_url_scheme_ok(const char *s);
/* Resolve the video path for playlist index `i` (wraps modulo the entry count). For a non-.txt
 * `src`, returns it verbatim for any i. Writes to `out`; returns the entry count (>=1), 0 on error. */
int bsdr_playlist_entry(const char *src, int i, char *out, size_t outlen);

/* discovery: record a Quest that broadcast (deduped by IP, refreshes last_seen) */
void bsdr_app_register_quest(bsdr_app *a, const char *ip);
/* selection gate for pairing: true if `ip` is the chosen Quest (or none chosen)
 * and not currently blocked by an operator disconnect */
bool bsdr_app_quest_allowed(bsdr_app *a, const char *ip);
/* refuse this Quest's pairing until it is reselected (operator disconnect) */
void bsdr_app_block_quest(bsdr_app *a, const char *ip);

void bsdr_app_set_paired(bsdr_app *a, bool paired, const char *name, const char *ip);
void bsdr_app_set_streaming(bsdr_app *a, bool streaming);
void bsdr_app_set_blank(bsdr_app *a, bool on);   /* privacy screen-blank toggle */
void bsdr_app_set_pointer_touch(bsdr_app *a, bool on);   /* input pointer mode: mouse vs real touch */
/* 2D->3D config (clamped; takes effect on the next capture reopen). ai_cmd may be NULL to keep. */
void bsdr_app_set_threed(bsdr_app *a, int mode, int deepness, int convergence, int swap, int full,
                         int tier, const char *ai_cmd);
void bsdr_app_get_threed(bsdr_app *a, int *mode, int *deepness, int *convergence, int *swap,
                         int *full, int *tier, char *ai_cmd, size_t ai_len);
void bsdr_app_set_cloud_mic_fallback(bsdr_app *a, bool on);
void bsdr_app_set_owner_mic_local(bsdr_app *a, bool on);
void bsdr_app_set_relay_port(bsdr_app *a, int port);
int  bsdr_app_get_relay_port(bsdr_app *a);
/* expose the cloud room's voice mix as a virtual mic "BSDR_RoomMic" (needs an active cloud session) */
void bsdr_app_set_room_mic(bsdr_app *a, bool on);
bool bsdr_app_get_room_mic(bsdr_app *a);
/* current Bigscreen access token (for the room-join REST call); empty when not logged in */
void bsdr_app_get_access_token(bsdr_app *a, char *out, size_t cap);
/* capture method for the Quest mic: 0 = passive sniff, 1 = MITM (ARP), 2 = router relay */
void bsdr_app_set_sniff_method(bsdr_app *a, int method);
int  bsdr_app_get_sniff_method(bsdr_app *a);
/* Quest-mic voice change: master on, gender (-100..100), robot/echo/whisper (0..100) + substitute */
void bsdr_app_set_voicefx(bsdr_app *a, bool on, int gender, int formant, int volume,
                          int robot, int echo, int whisper, bool substitute);
void bsdr_app_get_voicefx(bsdr_app *a, int *gender, int *formant, int *volume,
                          int *robot, int *echo, int *whisper, bool *substitute);

/* AI voice tier: enable + tier + target voice id + pitch key. Persisted; bumps voiceai_gen. */
void bsdr_app_set_voiceai(bsdr_app *a, bool on, int tier, const char *voice, int key);
void bsdr_app_get_voiceai(bsdr_app *a, bool *on, int *tier, char *voice, size_t vl, int *key);
void bsdr_app_set_voiceai_status(bsdr_app *a, const char *status);
/* Named voice presets (slots 0..4). save = snapshot the CURRENT changer state under `name`; apply =
 * load slot into the live state; delete = clear the slot. All persisted. */
void bsdr_app_voice_preset_save(bsdr_app *a, int slot, const char *name);
void bsdr_app_voice_preset_apply(bsdr_app *a, int slot);
void bsdr_app_voice_preset_delete(bsdr_app *a, int slot);
/* face swap: enable + tier + source-image path (bumps faceswap_gen for a live capture reopen) */
void bsdr_app_set_faceswap(bsdr_app *a, bool on, int tier, const char *source);
void bsdr_app_get_faceswap(bsdr_app *a, bool *on, int *tier, char *source, size_t sl);
void bsdr_app_set_faceswap_status(bsdr_app *a, const char *status);
void bsdr_app_set_room_pcm_sink(bsdr_app *a, void (*cb)(void *, const int16_t *, int, int), void *user);

/* cloud login (blocking HTTPS); updates cloud_* fields */
void bsdr_app_login(bsdr_app *a, const char *email, const char *password);
/* Restore a saved cloud session at startup (validate/renew the persisted token). Returns true
 * if we end up logged in (and the presence WS is (re)opened). */
bool bsdr_app_restore_session(bsdr_app *a);

/* clear cloud login state (token, name, email) */
void bsdr_app_logout(bsdr_app *a);

/* Second "bot" account (its own session/WS, no media). login/restore/logout mirror the host ones;
 * join_room makes the bot join the HOST's current room (so Room.participants > 1 unlocks the owner
 * mic). All blocking HTTPS, safe to call from the web-UI handler thread. */
void bsdr_app_bot_login(bsdr_app *a, const char *email, const char *password);
void bsdr_app_bot_restore(bsdr_app *a);
void bsdr_app_bot_logout(bsdr_app *a);
bool bsdr_app_bot_join_room(bsdr_app *a);
/* Leave the current room but stay logged in (WS presence up) — undo a Join. */
bool bsdr_app_bot_leave_room(bsdr_app *a);
/* Stop the bot: leave the room + drop the WS presence, but remember the login (one-click Start). */
void bsdr_app_bot_stop(bsdr_app *a);
/* Start (reconnect) a stopped bot from the remembered session — no password. */
void bsdr_app_bot_start(bsdr_app *a);
/* Set the bot presence mode: "audio" (REST join only) or "full" (avatar). Persisted. */
void bsdr_app_bot_set_mode(bsdr_app *a, const char *mode);
/* Follow-me: when on, the bot re-joins whatever room the operator moves into. Persisted. */
void bsdr_app_set_bot_follow(bsdr_app *a, bool on);
/* Cloud-mic loopback: route the bot's room audio (incl. your own voice) into BSDR_RoomMic. Persisted;
 * starts/stops the loopback immediately if the bot is joined. */
void bsdr_app_set_bot_loopback(bsdr_app *a, bool on);
/* "Listen only to me": solo the room owner's voice in the loopback (mute other participants). Persisted;
 * applied live. */
void bsdr_app_set_bot_solo_owner(bsdr_app *a, bool on);
/* Periodic follow-me reconcile (call ~every 15 s from the main loop). No-op unless follow is on and
 * the bot is logged in and not stopped. */
void bsdr_app_bot_follow_tick(bsdr_app *a);
/* Toggle the performance encoder preset (lighter CPU/GPU vs the default quality preset). Persisted;
 * takes effect on the next stream (re)start. */
void bsdr_app_set_enc_level(bsdr_app *a, int level);   /* 0 quality / 1 balanced / 2 performance */
void bsdr_app_set_x264_threads(bsdr_app *a, int n);    /* 0/1 single (default), >1 = N frame threads */
void bsdr_app_set_vaapi(bsdr_app *a, bool on);         /* --vaapi: iGPU VAAPI encode (Linux) */
void bsdr_app_set_kmsgrab(bsdr_app *a, bool on);       /* --kmsgrab: DRM/KMS capture (Linux) */
/* Send LAN video once (true) or twice (false, default). Persisted; takes effect live. */
void bsdr_app_set_lan_1x(bsdr_app *a, bool on);
/* Cap the desktop capture/encode fps (0 = default 30). Persisted; applies on the next stream start. */
void bsdr_app_set_fps_cap(bsdr_app *a, int fps);
/* Enable Wi-Fi network optimization (DSCP/WMM priority marking on the LAN media). Persisted; applies
 * on the next stream start. */
void bsdr_app_set_wifi_opt(bsdr_app *a, bool on);

/* internet sharing (Mediasoup relay) toggle. On enable: ensure WS presence is up,
 * GET /rooms for a relay assignment, and (if found) begin relay streaming.
 * On disable: stop relay streaming. Returns the resolved relay screen (or found=false). */
struct bsdr_cloud_screen;
void bsdr_app_set_internet_sharing(bsdr_app *a, bool on);
bool bsdr_app_get_internet_sharing(bsdr_app *a);
/* Poll for a Quest-provisioned screen and start the relay stream once it exists.
 * Call periodically (the agent does, ~every few seconds) while sharing is on. */
void bsdr_app_cloud_tick(bsdr_app *a);
/* Unpair grace: set_paired(false) does NOT stop the relay immediately — it arms a grace timer so a
 * quick unpair/re-pair (or a transient heartbeat gap) keeps the internet-share stream alive. Call
 * bsdr_app_unpair_grace_expired() each loop; it stops the relay + clears sharing once the timer lapses
 * with no re-pair (returns true that tick). bsdr_app_unpair_now() finalizes immediately (deliberate
 * operator disconnect). A subsequent set_paired(true) cancels the pending teardown. */
bool bsdr_app_unpair_grace_expired(bsdr_app *a);
void bsdr_app_unpair_now(bsdr_app *a);

/* The single LAN encoder feeds each encoded access unit here (w/h = encoded resolution); forwarded
 * to the relay as plain RTP in COUPLED mode. No-op in --video-decoupled mode (relay self-captures). */
void bsdr_app_feed_cloud_video(bsdr_app *a, const uint8_t *au, size_t len, int w, int h);

/* setters from the web UI */
void bsdr_app_select_quest(bsdr_app *a, const char *ip);
unsigned bsdr_app_select_gen(bsdr_app *a);
void bsdr_app_set_source(bsdr_app *a, const char *mode, const char *path);
void bsdr_app_set_file_loop(bsdr_app *a, int on);   /* loop the file/playlist continuously (persisted) */
/* Stereo webcam: set/get the right-eye camera device (used when source mode is "webcam3d"). */
void bsdr_app_set_source_right(bsdr_app *a, const char *dev);
void bsdr_app_get_source_right(bsdr_app *a, char *dev, size_t dl);
void bsdr_app_set_paused(bsdr_app *a, bool paused);
bool bsdr_app_is_paused(bsdr_app *a);
/* operator-initiated disconnect: request from the web UI, consume in the main loop */
void bsdr_app_request_disconnect(bsdr_app *a);
bool bsdr_app_take_disconnect(bsdr_app *a);

/* owner-mic sniffer: web UI sets the desired state (+ transient sudo password); the main loop
 * consumes the dirty flag and reconciles by starting/stopping the sniffer. */
void bsdr_app_set_sniff(bsdr_app *a, bool want, const char *password);
/* If the desired state changed since the last call, copy it out (and the password, then wipe it
 * from the app) and return true. `pw` must be >= 128 bytes. */
bool bsdr_app_take_sniff(bsdr_app *a, bool *want, bool *mitm, char *pw, size_t pwlen);
void bsdr_app_set_sniff_status(bsdr_app *a, bool active, const char *msg);
void bsdr_app_get_source(bsdr_app *a, char *mode, size_t ml, char *path, size_t pl);
/* Terminal-source backend + pty grid size. Set persists across restarts. backend "pty"|"xvfb"
 * (NULL/empty keeps current); cols/rows <= 0 keep current. */
void bsdr_app_set_terminal(bsdr_app *a, const char *backend, int cols, int rows);
void bsdr_app_get_terminal(bsdr_app *a, char *backend, size_t bl, int *cols, int *rows);
void bsdr_app_set_quality(bsdr_app *a, int w, int h, int bitrate);
void bsdr_app_get_quality(bsdr_app *a, int *w, int *h, int *bitrate);
/* Web-UI bitrate override: bps to force, or 0 to follow whatever the headset asks for. Recomputes
 * the effective bitrate immediately; the live streamer reopens the encoder on the next tick. */
void bsdr_app_set_bitrate_override(bsdr_app *a, int bitrate);
int  bsdr_app_get_bitrate_override(bsdr_app *a);
/* Encoder choice (gpu=true -> NVENC/CUDA; false -> libx264). Live-switchable; persisted. */
void bsdr_app_set_gpu_encode(bsdr_app *a, bool gpu);
bool bsdr_app_get_gpu_encode(bsdr_app *a);
/* Load persisted user prefs (encoder, bitrate override) at startup — call after bsdr_app_init and
 * before applying CLI flags so an explicit flag still wins. */
void bsdr_app_load_settings(bsdr_app *a);
/* Per-user config directory ($XDG_CONFIG_HOME/bsdr_agent or ~/.config/bsdr_agent), created if needed.
 * Exposed so cloud_stream can persist sticky source ports next to the other settings. */
bool bsdr_config_dir(char *dir, size_t cap);
void bsdr_app_set_region(bsdr_app *a, int x, int y, int w, int h);
void bsdr_app_get_region(bsdr_app *a, int *x, int *y, int *w, int *h);
/* voice config: each field may be "" to leave unchanged on set. get copies all. */
void bsdr_app_set_voice(bsdr_app *a, const char *se, const char *st, const char *sm,
                        const char *le, const char *lt, const char *lm);
void bsdr_app_get_voice(bsdr_app *a, char *se, char *st, char *sm,
                        char *le, char *lt, char *lm, size_t each);

/* computer control: web UI sets the desired state; the main loop consumes the
 * dirty flag and arms/disarms the voice pipeline. `vision` also feeds a desktop
 * screenshot to the model. */
void bsdr_app_set_compctl(bsdr_app *a, bool want, bool vision);
bool bsdr_app_take_compctl(bsdr_app *a, bool *want, bool *vision);
void bsdr_app_set_compctl_status(bsdr_app *a, bool active, const char *msg);

/* serialize the whole state as JSON for /api/status */
size_t bsdr_app_status_json(bsdr_app *a, char *out, size_t cap);

#endif /* BSDR_APP_H */
