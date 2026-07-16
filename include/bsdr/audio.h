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
/* Audio: Opus over RTP/SRTP (RFC 7587), plus PulseAudio capture/playback and
 * virtual sink/source management. Built with BSDR_ENABLE_AUDIO (Linux).
 *
 * Desktop out: capture the system audio (via a null-sink monitor; the sink is
 *   made default so the speakers go silent) -> Opus 48k stereo 64k -> SRTP -> Quest.
 * Mic in: Quest mic SRTP -> Opus decode -> a virtual source the system sees as a
 *   microphone.
 */
#ifndef BSDR_AUDIO_H
#define BSDR_AUDIO_H

#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BSDR_OPUS_FRAME 960        /* 20 ms @ 48 kHz, per channel */

/* Windows virtual-mic endpoint name: the installer renames VB-CABLE's endpoints to this so apps
 * see a "BSRD_Mic" microphone. Shared by the WASAPI backend and the owner-mic sniffer. */
#ifndef BSDR_MIC_DEVICE_NAME
#define BSDR_MIC_DEVICE_NAME "BSRD_Mic"
#endif

/* --- Opus RTP/SRTP sender (desktop audio -> Quest) ------------------------*/
typedef struct bsdr_audio_sender bsdr_audio_sender;
bsdr_audio_sender *bsdr_audio_sender_new(bsdr_udp *udp, const bsdr_srtp_keys *keys,
                                         uint8_t pt, uint32_t ssrc,
                                         int channels, int bitrate);
/* Encode + send one frame of interleaved int16 PCM (`frames` samples/channel). */
int bsdr_audio_send_pcm(bsdr_audio_sender *s, const int16_t *pcm, int frames);
void bsdr_audio_sender_set_gain(bsdr_audio_sender *s, int vol_0_100);
/* Cloud relay desktop audio only: append the 8-byte BigSoup trailer ([u32 ssrc LE][u32 frame_id LE],
 * no XOR) after the Opus payload so the Quest reads the producer SSRC instead of 0 (which crashes it). */
void bsdr_audio_sender_enable_cloud_trailer(bsdr_audio_sender *s);
/* Send each packet N times (real Bigscreen room mic sends 2×; plain-RTP path only). */
void bsdr_audio_sender_set_dup(bsdr_audio_sender *s, int n);
void bsdr_audio_sender_free(bsdr_audio_sender *s);

/* --- Opus RTP/SRTP receiver (Quest mic -> system) -------------------------*/
typedef void (*bsdr_pcm_cb)(const int16_t *pcm, int frames, int channels, void *user);
typedef struct bsdr_audio_recv bsdr_audio_recv;
bsdr_audio_recv *bsdr_audio_recv_new(const bsdr_srtp_keys *keys, uint8_t pt,
                                     uint32_t ssrc, int channels,
                                     bsdr_pcm_cb cb, void *user);
/* Feed one inbound RTP/SRTP packet into its per-SSRC jitter buffer (no decode here). */
int bsdr_audio_recv_feed(bsdr_audio_recv *r, uint8_t *srtp_pkt, int len);
/* Play out one mixed 20 ms frame (decode next in-order packet per stream, or PLC; sum -> cb).
 * Call on a ~20 ms clock. Returns 1 if a frame was emitted, 0 if nothing was ready. */
int bsdr_audio_recv_playout(bsdr_audio_recv *r);
/* Voice-activity duck: when on, playout mixes ONLY the loudest active stream and mutes the rest
 * (owner isolation while a voice command is captured over the cloud room-audio fallback). */
void bsdr_audio_recv_set_duck(bsdr_audio_recv *r, int on);
/* Identity solo: when ssrc != 0, playout mixes ONLY the stream with that SSRC (mute everyone else) —
 * used to "listen only to me" (the room owner) via cloud_ssrc(ownerSessionId). 0 = mix all. */
void bsdr_audio_recv_set_solo(bsdr_audio_recv *r, uint32_t ssrc);

/* Per-SSRC volume policy: scale each stream's contribution to the mix by a gain (0.0 = muted,
 * 1.0 = unity). Used by the bot's room-audio policy — owner loudest, host/friend slightly lower,
 * everyone else silenced — keyed to each speaker's access level. set_gain / set_gain_default
 * activate gain mode; until then (and after clear_gains) every stream plays at unity, so existing
 * consumers are unaffected. Composes with solo/duck (those still choose which streams are included;
 * gain then scales the included ones). */
void bsdr_audio_recv_set_gain(bsdr_audio_recv *r, uint32_t ssrc, float gain);
void bsdr_audio_recv_set_gain_default(bsdr_audio_recv *r, float gain);   /* for unlisted SSRCs */
void bsdr_audio_recv_clear_gains(bsdr_audio_recv *r);                    /* reset + back to unity */

/* Per-SSRC PCM tap: fires once per decoded 20 ms frame for every AUDIBLE stream (effective gain > 0)
 * with that stream's raw (pre-gain) PCM and its SSRC — so a consumer can run per-speaker VAD/STT and
 * map the SSRC back to a participant. NULL cb disables it. Independent of the mixed-output cb. */
typedef void (*bsdr_ssrc_pcm_cb)(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user);
void bsdr_audio_recv_set_tap(bsdr_audio_recv *r, bsdr_ssrc_pcm_cb cb, void *user);
/* Strip the 8-byte BigSoup cloud trailer ([u32 ssrc][u32 frame_id]) before Opus decode — the room
 * audio the SFU sends back on the audio port carries it (as the sender appends). */
void bsdr_audio_recv_enable_cloud_trailer(bsdr_audio_recv *r);
void bsdr_audio_recv_free(bsdr_audio_recv *r);

/* --- PulseAudio I/O (libpulse simple) -------------------------------------*/
typedef struct bsdr_pa bsdr_pa;
/* Record from `source` (e.g. "<sink>.monitor"); NULL = default. */
bsdr_pa *bsdr_pa_record_open(const char *source, int channels);
int bsdr_pa_read(bsdr_pa *pa, int16_t *pcm, int frames);     /* blocking */
/* Play into `sink`; NULL = default. */
bsdr_pa *bsdr_pa_play_open(const char *sink, int channels);
bsdr_pa *bsdr_pa_play_open_quiet(const char *sink, int channels);   /* no ERROR log if the sink is absent */
int bsdr_pa_write(bsdr_pa *pa, const int16_t *pcm, int frames);
void bsdr_pa_close(bsdr_pa *pa);

/* Threaded playback queue: push PCM from any thread (non-blocking, drops on
 * overflow); a worker drains it to `sink`. For the inbound mic so decode never
 * blocks the receive pump. */
typedef struct bsdr_audio_player bsdr_audio_player;
bsdr_audio_player *bsdr_audio_player_new(const char *sink, int channels);
/* Like _new but silent if the sink isn't present (returns NULL, no ERROR log) — for lazy/optional
 * opens such as the owner-mic sniffer polling for the session-scoped virtual mic sink. */
bsdr_audio_player *bsdr_audio_player_new_quiet(const char *sink, int channels);
/* True once the sink the player writes to has gone away (writes fail) — the caller should free and
 * reopen it (the virtual mic sink is torn down when a streaming session ends). */
int bsdr_audio_player_dead(const bsdr_audio_player *p);
void bsdr_audio_player_push(bsdr_audio_player *p, const int16_t *pcm, int frames);
void bsdr_audio_player_free(bsdr_audio_player *p);

/* --- virtual devices (pactl) ----------------------------------------------*/
/* Create a null-sink "BSDR-Quest" (made default => speakers silent; we capture
 * its monitor) and a virtual mic "BSDR-Quest-Mic" (we feed it decoded audio).
 * Fills monitor_source / mic_sink names. Returns false if pactl/modules fail. */
typedef struct {
    int speaker_module, mic_sink_module, mic_source_module;
    char prev_default_sink[256];
    char monitor_source[128];   /* record the desktop audio from here */
    char mic_sink[128];         /* play decoded Quest mic into here */
    bool active;
} bsdr_audio_devices;
bool bsdr_audio_devices_create(bsdr_audio_devices *d);
void bsdr_audio_devices_destroy(bsdr_audio_devices *d);

/* Standalone virtual microphone (independent of bsdr_audio_devices): a null-sink whose
 * monitor is remapped to a capture source apps can pick. Used by the owner-mic sniffer for
 * a device separate from BSDR-Quest-Mic. Returns false off Linux or if pactl fails; on
 * success fills the two module ids for bsdr_virtual_mic_destroy. */
bool bsdr_virtual_mic_create(const char *sink_name, const char *source_name,
                             const char *description, int *sink_module, int *source_module);
void bsdr_virtual_mic_destroy(int sink_module, int source_module);
/* Is a PulseAudio sink with this name currently loaded? Lets a caller avoid creating a second sink of
 * the same name (e.g. the owner-mic relay reusing the streaming session's bsdr_micsink). 0 off Linux.
 * Bring up the PERSISTENT BSDR_QuestMic device (bsdr_micsink + remap-source) if it isn't already, and
 * keep it for the process lifetime — recording apps bind to it once, across streaming sessions. Idempotent
 * and safe to call from the streaming-session bring-up or the owner-mic relay. Both are real functions in
 * every audio build (Linux implements them; win/osx/android provide no-op parity stubs — those platforms
 * use a persistent loopback device instead) and inline no-ops in input-only builds. */
#ifdef BSDR_HAVE_AUDIO
int  bsdr_audio_sink_exists(const char *sink_name);
void bsdr_audio_questmic_ensure(void);
#else
static inline int  bsdr_audio_sink_exists(const char *sink_name) { (void)sink_name; return 0; }
static inline void bsdr_audio_questmic_ensure(void) {}
#endif
/* Unload any leftover bsdr_* virtual-audio modules (from a prior run that didn't clean up).
 * Safe to call any time — at startup, on exit, or from a signal/atexit handler. No-op off Linux.
 * audio.c is only compiled in audio builds; input-only builds (make linux-static, plain windows)
 * get an inert stub so the shared agent flow — which calls this from signal/atexit paths — links. */
#ifdef BSDR_HAVE_AUDIO
void bsdr_audio_cleanup_stale_devices(void);
#else
static inline void bsdr_audio_cleanup_stale_devices(void) {}
#endif

#endif /* BSDR_AUDIO_H */
