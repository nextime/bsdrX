/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Bot -> cloud ROOM MIC producer: publish audio INTO the Bigscreen room as the second-account bot, so
 * everyone in the room hears it. This is the SEND counterpart of botaudio.c (which only consumes the
 * room mix). It's the transport the TTS / "speak" output rides to the room by default.
 *
 * Wire format = the same unencrypted mediasoup voice producer the Quest uses (reversed, proven for the
 * companion's desktop audio): plain Opus RTP, PT 100, 48 kHz MONO, SSRC = djb2(the bot's own
 * userSessionId) (bsdr_cloud_user_ssrc), + the 8-byte BigSoup cloud trailer [u32 ssrc LE][u32 frame_id
 * LE] after the Opus payload. Sent to the bot's OWN mediaPeer.micPort on the relay (comedia: the relay
 * learns our address from the first packet). No DTLS/SRTP on the room media plane.
 *
 * A keepalive thread streams continuous 10 ms Opus silence between utterances (like a real client),
 * paced to wall-clock so the producer's RTP timestamp tracks real time — see botmic.c. Built only when
 * audio/media is compiled in; otherwise start() returns NULL. */
#ifndef BSDR_BOTMIC_H
#define BSDR_BOTMIC_H

#include <stdint.h>

typedef struct bsdr_botmic bsdr_botmic;

/* Open the producer to relay_ip:mic_port (the bot's own room mediaPeer micPort) with SSRC derived from
 * session_id (the bot's own userSessionId). `keepalive_on` (borrowed, may be NULL) is polled live: when
 * it points to a non-zero value the producer streams continuous silence between utterances (the crash
 * fix). NULL => always continuous. The keepalive cadence defaults to a correct 10 ms/10 ms real-time
 * pace; a loadable plugin may override it live (see bsdr_plugins_mic_keepalive_period_ms). Returns NULL
 * if media isn't built or inputs are empty. */
bsdr_botmic *bsdr_botmic_start(const char *relay_ip, int mic_port, const char *session_id,
                               const volatile int *keepalive_on);

/* Send one frame of 48 kHz MONO PCM into the room (thread-safe). frames = samples (e.g. 960 = 20 ms).
 * No-op / -1 on a NULL handle. */
int bsdr_botmic_push(bsdr_botmic *b, const int16_t *pcm, int frames);

/* Stop + free (idempotent; NULL-safe). */
void bsdr_botmic_stop(bsdr_botmic *b);

#endif /* BSDR_BOTMIC_H */
