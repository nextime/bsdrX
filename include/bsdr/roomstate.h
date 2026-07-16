/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Room/avatar FlatBuffers state codec (ported from bsbot; schema src/generated/room_state.fbs).
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* Room/avatar data-channel codec for the Bigscreen Quest relay. Reverse-engineered byte-for-byte
 * from the live Quest APK (libil2cpp/libBigSoup) and validated against a real room.pcap — see
 * DataChannel/OnData. The old bsbot "[1 type byte][FlatBuffer]" framing was WRONG at three layers;
 * the real on-wire SCTP payload (stream 1, ordered) is an ASCII string:
 *
 *     "<legacyUserId>" '*' base64( <4-byte LE DataChannelDataType> <body> )
 *
 *   - body(UserState) = a UserState FlatBuffer whose Avatar field is MANDATORY (the receiver calls
 *     Nullable<AvatarState>.get_Value() unconditionally; an absent Avatar throws → no avatar renders).
 *   - body(TickState) = a raw 176-byte marshalled struct (NOT a FlatBuffer).
 *   - legacyUserId is the sender's short room id ("userNNN"); it must equal the id the room roster
 *     registered for us, and it is also written into UserState.user_id. */
#ifndef BSBOT_ROOMSTATE_H
#define BSBOT_ROOMSTATE_H

#include <stddef.h>

/* DataChannelDataType (Bigscreen.Networking.DataChannel enum, verified) — the 4-byte LE code. */
#define RS_TYPE_UNKNOWN    0
#define RS_TYPE_USER_STATE 1
#define RS_TYPE_TICK_STATE 2
#define RS_TYPE_MONITOR    3
#define RS_TYPE_POSE       4

typedef struct { float pos[3]; float rot[4]; } rs_pose; /* PoseState: Vector3 pos + Quaternion rot */

/* Encode a full SCTP data-channel payload ("<legacyUserId>*base64(code+body)") into a malloc'd
 * buffer (caller free()s). legacy_user_id is the bot's short room id used for BOTH the wire prefix
 * and the UserState.user_id field. Returns 0 on success. */
int roomstate_user_state(char **out, size_t *out_len, const char *legacy_user_id,
                         int area_index, int seat_index);
int roomstate_tick_state(char **out, size_t *out_len, const char *legacy_user_id,
                         float mic_loudness, const rs_pose *head);

/* Decoded view of an inbound frame (enough for seat/pose + avatar hands/eyes). */
typedef struct {
    int type;                 /* RS_TYPE_* */
    char user_id[160];        /* UserState.user_id */
    int area_index, seat_index;
    int body_type_index;      /* UserState.Avatar.body_type_index (0=Smol,1=Basic,2=Chonk,3=Stronk) */
    float mic_loudness;       /* TickState */
    rs_pose head;
    int has_head;
    /* TickState hands (HandState.pose) + eye gaze (irisOffset). has_* mirror HandState.showingHand. */
    rs_pose left_hand, right_hand;
    int has_left, has_right;
    float iris[2];            /* irisOffset (Vector2) — eye gaze */
} rs_decoded;

/* Decode a framed message. Returns the type (>=0) or -1 on error. */
int roomstate_decode(const void *frame, size_t len, rs_decoded *out);

int roomstate_init(void);

#endif /* BSBOT_ROOMSTATE_H */
