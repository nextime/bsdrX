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
/* Cloud-mic loopback via the second-account bot.
 *
 * The room's mediasoup SFU does NOT send your own voice back to you, and a producer with no consumers
 * may be dropped — so bsdrX's own room mic (cloud_mic_main) never carries the operator's voice and
 * needs someone else in the room. The BOT, being a separate participant, IS sent everyone else's
 * audio — INCLUDING the operator's. This consumer opens the bot's own room audio port, pulls that mix
 * and renders it into the same virtual mic BSDR_RoomMic, so the operator's own voice reaches the
 * computer and the room mic works even when alone.
 *
 * "Listen only to me": the room owner's audio SSRC is djb2(ownerSessionId) (bsdr_cloud_user_ssrc), so
 * we can solo it and mute other participants (and the owner's desktop audio, a different SSRC).
 *
 * Built only when SCTP/media is compiled in; otherwise start() returns NULL. */
#ifndef BSDR_BOTAUDIO_H
#define BSDR_BOTAUDIO_H

#include "bsdr/app.h"

typedef struct bsdr_botaudio bsdr_botaudio;

/* Start consuming the bot's room audio (relay_ip:audio_port from the bot's room-join mediaPeer) and
 * render it into BSDR_RoomMic. owner_session_id = the room owner's userSessionId (for the "listen only
 * to me" solo; may be empty). The thread live-reads app->room_mic_want (device on/off) and
 * app->bot_solo_owner (solo on/off), and sets app->bot_roommic_active while it owns the device so the
 * host's cloud_mic_main defers. Returns NULL if media isn't built or inputs are empty. */
bsdr_botaudio *bsdr_botaudio_start(const char *relay_ip, int audio_port,
                                   const char *owner_session_id, bsdr_app *app);

/* Stop + join (idempotent; NULL-safe). Clears app->bot_roommic_active. */
void bsdr_botaudio_stop(bsdr_botaudio *b);

#endif /* BSDR_BOTAUDIO_H */
