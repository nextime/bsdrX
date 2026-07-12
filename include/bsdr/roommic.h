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
/* Shared definition of the ROOM_SINK — the per-platform audio sink the decoded Bigscreen ROOM audio
 * is rendered into and re-exposed as the virtual mic BSDR_RoomMic. Two sources can drive it:
 *   - cloud_stream.c (cloud_mic_main): the HOST's internet-sharing connection (others only, no self),
 *   - botaudio.c: the second-account BOT's room consume (others + your OWN voice, works solo).
 * Only one owns the device at a time (the bot takes precedence when its loopback is on). */
#ifndef BSDR_ROOMMIC_H
#define BSDR_ROOMMIC_H

#if defined(__APPLE__)
#  define ROOM_SINK "BlackHole"
#elif defined(_WIN32)
#  define ROOM_SINK BSDR_MIC_DEVICE_NAME
#else
#  define ROOM_SINK "bsdr_roomsink"
#endif

/* The virtual-mic source/device names BSDR_RoomMic is created under (bsdr_virtual_mic_create). */
#define ROOM_MIC_SINK_NAME  "bsdr_room_mic"
#define ROOM_MIC_SRC_NAME   "BSDR_RoomMic"
#define ROOM_MIC_LABEL      "BSDR_RoomMic"

#endif /* BSDR_ROOMMIC_H */
