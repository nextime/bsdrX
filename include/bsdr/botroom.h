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
/* Full-bot avatar presence: the room data plane for the second-account bot.
 *
 * "audio-only" bot = REST join only (raises Room.participants so the owner mic unlocks). The bot is
 * a userlist ghost with no avatar. "full" bot additionally connects the room's mediasoup data
 * channel (raw usrsctp over the join's mediaPeer dataPort, no DTLS/DCEP) and broadcasts the bot's
 * UserState once + a periodic TickState head pose so an avatar renders for everyone in the room. The
 * wire format (string "legacyId*base64(code+body)", mandatory Avatar, raw 176-byte TickState, PPID
 * 33 00 00 00) is reversed byte-for-byte from the live Quest APK — see roomstate.c/.h.
 *
 * Built only when SCTP is compiled in (BSDR_ENABLE_SCTP); otherwise start() returns NULL and the bot
 * silently stays audio-only. */
#ifndef BSDR_BOTROOM_H
#define BSDR_BOTROOM_H

typedef struct bsdr_botroom bsdr_botroom;

/* Live state of the avatar data plane, polled by the UI so a join shows real progress instead of a
 * premature "avatar up" (the SCTP association completes seconds after the thread starts, or fails). */
typedef enum {
    BSDR_AVATAR_OFF        = 0,   /* no presence thread (audio-only / stopped / not built) */
    BSDR_AVATAR_CONNECTING = 1,   /* SCTP INIT sent, retrying to associate with the relay */
    BSDR_AVATAR_UP         = 2,   /* associated; broadcasting UserState + pose (avatar renders) */
    BSDR_AVATAR_GHOST      = 3    /* gave up associating — userlist ghost, no avatar */
} bsdr_avatar_state;

/* Current avatar-plane state (NULL-safe -> BSDR_AVATAR_OFF). */
bsdr_avatar_state bsdr_botroom_avatar_state(const bsdr_botroom *b);

/* Start the avatar presence for a joined room. relay_ip:data_port is the bot's own MediaPeer from the
 * room-join response; legacy_user_id = the bot's room legacyUserId ("userNNN") — the exact string the
 * Quest keys remote avatars by, so it MUST match the roster (an empty/wrong id renders nothing);
 * seat_index < 0 = free-standing. Runs on its own thread. Returns NULL if SCTP isn't built or inputs
 * are empty. */
bsdr_botroom *bsdr_botroom_start(const char *relay_ip, int data_port,
                                 const char *legacy_user_id, int seat_index);

/* Stop + join the presence thread (idempotent; NULL-safe). */
void bsdr_botroom_stop(bsdr_botroom *b);

#endif /* BSDR_BOTROOM_H */
