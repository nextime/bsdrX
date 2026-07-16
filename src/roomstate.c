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
#include "bsdr/roomstate.h"
#include "bsdr/log.h"

#include "flatcc/flatcc_builder.h"
#include "room_state_builder.h"
#include "room_state_reader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* shorten the generated namespace */
#undef ns
#define ns(x) Bigscreen_Flatbuffer_##x

/* ---- base64 (standard alphabet, '=' padding) — matches Convert.ToBase64String in BigSoup ------- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *in, size_t n, size_t *out_len) {
    size_t olen = 4 * ((n + 2) / 3);
    char *o = malloc(olen + 1);
    if (!o) return NULL;
    size_t i, j;
    for (i = 0, j = 0; i + 2 < n; i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        o[j++] = B64[(v >> 18) & 63]; o[j++] = B64[(v >> 12) & 63];
        o[j++] = B64[(v >> 6) & 63];  o[j++] = B64[v & 63];
    }
    if (i < n) {                                   /* 1 or 2 trailing bytes */
        uint32_t v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        o[j++] = B64[(v >> 18) & 63];
        o[j++] = B64[(v >> 12) & 63];
        o[j++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        o[j++] = '=';
    }
    o[j] = '\0';
    if (out_len) *out_len = j;
    return o;
}

/* base64 decode (for the inbound peer-state log). Returns malloc'd bytes or NULL; *out_len set. */
static uint8_t *b64_decode(const char *in, size_t n, size_t *out_len) {
    static int8_t T[256]; static int init = 0;
    if (!init) { memset(T, -1, sizeof T); for (int k = 0; k < 64; k++) T[(unsigned char)B64[k]] = (int8_t)k; init = 1; }
    uint8_t *o = malloc(n / 4 * 3 + 3); if (!o) return NULL;
    size_t j = 0; int q[4], qi = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' ) break;
        int8_t d = T[c]; if (d < 0) continue;      /* skip whitespace/newlines */
        q[qi++] = d;
        if (qi == 4) { o[j++] = (q[0] << 2) | (q[1] >> 4); o[j++] = (q[1] << 4) | (q[2] >> 2); o[j++] = (q[2] << 6) | q[3]; qi = 0; }
    }
    if (qi >= 2) { o[j++] = (q[0] << 2) | (q[1] >> 4); if (qi == 3) o[j++] = (q[1] << 4) | (q[2] >> 2); }
    if (out_len) *out_len = j;
    return o;
}

/* Wrap [code(LE32)][body] as the wire payload "<legacyId>*base64(...)" -> *out (malloc), *out_len. */
static int wire_frame(char **out, size_t *out_len, const char *legacy_user_id,
                      int code, const void *body, size_t body_len) {
    size_t raw = 4 + body_len;
    uint8_t *tmp = malloc(raw ? raw : 1);
    if (!tmp) return -1;
    tmp[0] = (uint8_t)(code & 0xff); tmp[1] = (uint8_t)((code >> 8) & 0xff);
    tmp[2] = (uint8_t)((code >> 16) & 0xff); tmp[3] = (uint8_t)((code >> 24) & 0xff);
    if (body_len) memcpy(tmp + 4, body, body_len);
    size_t b64len = 0;
    char *b64 = b64_encode(tmp, raw, &b64len);
    free(tmp);
    if (!b64) return -1;
    const char *id = legacy_user_id ? legacy_user_id : "";
    size_t idlen = strlen(id);
    char *o = malloc(idlen + 1 + b64len + 1);
    if (!o) { free(b64); return -1; }
    memcpy(o, id, idlen); o[idlen] = '*';
    memcpy(o + idlen + 1, b64, b64len); o[idlen + 1 + b64len] = '\0';
    free(b64);
    *out = o; *out_len = idlen + 1 + b64len;
    return 0;
}

int roomstate_user_state(char **out, size_t *out_len, const char *legacy_user_id,
                         int area_index, int seat_index) {
    flatcc_builder_t B;
    flatcc_builder_init(&B);
    /* Nested Avatar first (flatcc: child tables must be finished before the parent opens). All 16
     * appearance indices default to 0 — a valid default avatar; accessory_states left empty. Its
     * PRESENCE is what matters (the receiver dereferences UserState.Avatar unconditionally). */
    ns(AvatarState_start)(&B);
    ns(AvatarState_ref_t) avatar = ns(AvatarState_end)(&B);

    ns(UserState_start_as_root)(&B);
    ns(UserState_user_id_add)(&B, flatbuffers_string_create_str(&B, legacy_user_id ? legacy_user_id : ""));
    ns(UserState_area_index_add)(&B, area_index);
    ns(UserState_seat_index_add)(&B, seat_index);
    ns(UserState_avatar_add)(&B, avatar);          /* MANDATORY */
    ns(UserState_wearing_hmd_add)(&B, 1);
    ns(UserState_showing_head_add)(&B, 1);
    ns(UserState_end_as_root)(&B);

    size_t sz;
    void *fb = flatcc_builder_finalize_aligned_buffer(&B, &sz);
    int rc = fb ? wire_frame(out, out_len, legacy_user_id, RS_TYPE_USER_STATE, fb, sz) : -1;
    flatcc_builder_aligned_free(fb);
    flatcc_builder_clear(&B);
    return rc;
}

/* Write a 32-bit float into a byte buffer at a fixed offset (little-endian, matches the marshalled
 * blittable C# struct). */
static void put_f32(uint8_t *p, size_t off, float f) { memcpy(p + off, &f, 4); }

int roomstate_tick_state(char **out, size_t *out_len, const char *legacy_user_id,
                         float mic_loudness, const rs_pose *head) {
    /* TickState is NOT a FlatBuffer — it's a raw 176-byte marshalled struct (Marshal.SizeOf + MemCpy).
     * Layout (dump.cs TypeDefIndex 8905): micLoudness@0x00, headPose(PoseState 28B)@0x04,
     * leftHand(HandState 68B)@0x20, rightHand@0x64, irisOffset(Vector2 8B)@0xA8. HandState.showingHand
     * is byte 0 of each hand block; leaving the hand blocks zeroed hides the (unused) bot hands. */
    uint8_t ts[176];
    memset(ts, 0, sizeof ts);
    put_f32(ts, 0x00, mic_loudness);
    if (head) {
        put_f32(ts, 0x04, head->pos[0]); put_f32(ts, 0x08, head->pos[1]); put_f32(ts, 0x0C, head->pos[2]);
        put_f32(ts, 0x10, head->rot[0]); put_f32(ts, 0x14, head->rot[1]);
        put_f32(ts, 0x18, head->rot[2]); put_f32(ts, 0x1C, head->rot[3]);
    } else {
        put_f32(ts, 0x1C, 1.0f);          /* identity quaternion w */
    }
    return wire_frame(out, out_len, legacy_user_id, RS_TYPE_TICK_STATE, ts, sizeof ts);
}

int roomstate_decode(const void *frame_in, size_t len, rs_decoded *out) {
    if (!frame_in || len < 2 || !out) return -1;
    memset(out, 0, sizeof(*out));
    const char *p = (const char *)frame_in;
    /* Inbound is "<legacyId>*base64(code+body)". Split on '*', capture the sender's legacy id, then
     * base64-decode the tail; the first 4 bytes are the LE DataChannelDataType code. */
    const char *star = memchr(p, '*', len);
    if (!star) return -1;
    size_t idn = (size_t)(star - p);
    if (idn >= sizeof(out->user_id)) idn = sizeof(out->user_id) - 1;
    memcpy(out->user_id, p, idn);         /* provisional: the wire legacy id (UserState may override) */
    out->user_id[idn] = '\0';

    size_t b64n = len - (idn + 1);
    size_t raw_len = 0;
    uint8_t *raw = b64_decode(star + 1, b64n, &raw_len);
    if (!raw || raw_len < 4) { free(raw); return -1; }
    out->type = (int)((uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24));

    uint8_t *body = raw + 4;
    size_t body_len = raw_len - 4;
    int rc = out->type;
    if (out->type == RS_TYPE_USER_STATE) {
        /* Copy to a fresh malloc-aligned buffer so flatcc's aligned reads are valid. */
        void *fb = malloc(body_len ? body_len : 1);
        if (fb) {
            memcpy(fb, body, body_len);
            ns(UserState_table_t) us = ns(UserState_as_root)(fb);
            if (us) {
                flatbuffers_string_t uid = ns(UserState_user_id)(us);
                if (uid) {
                    size_t n = flatbuffers_string_len(uid);
                    if (n >= sizeof(out->user_id)) n = sizeof(out->user_id) - 1;
                    memcpy(out->user_id, uid, n);
                    out->user_id[n] = '\0';
                }
                out->area_index = ns(UserState_area_index)(us);
                out->seat_index = ns(UserState_seat_index)(us);
                ns(AvatarState_table_t) av = ns(UserState_avatar)(us);   /* nested Avatar (mandatory) */
                if (av) out->body_type_index = ns(AvatarState_body_type_index)(av);
            }
            free(fb);
        }
    } else if (out->type == RS_TYPE_TICK_STATE && body_len >= 0x20) {
        /* Raw 176-byte struct: micLoudness@0x00, headPose(pos@0x04, rot@0x10). */
        memcpy(&out->mic_loudness, body + 0x00, 4);
        memcpy(&out->head.pos[0], body + 0x04, 4); memcpy(&out->head.pos[1], body + 0x08, 4);
        memcpy(&out->head.pos[2], body + 0x0C, 4);
        memcpy(&out->head.rot[0], body + 0x10, 4); memcpy(&out->head.rot[1], body + 0x14, 4);
        memcpy(&out->head.rot[2], body + 0x18, 4); memcpy(&out->head.rot[3], body + 0x1C, 4);
        out->has_head = 1;
        /* Hands (HandState @0x20 left / @0x64 right): showingHand@+0, pose.pos@+0x14, pose.rot@+0x20;
         * eye gaze irisOffset(Vector2)@0xA8. (Struct offsets from the decompiled HandState/PoseState.) */
        if (body_len >= 0xB0) {
            out->has_left  = body[0x20] != 0;
            memcpy(out->left_hand.pos, body + 0x34, 12);  memcpy(out->left_hand.rot, body + 0x40, 16);
            out->has_right = body[0x64] != 0;
            memcpy(out->right_hand.pos, body + 0x78, 12); memcpy(out->right_hand.rot, body + 0x84, 16);
            memcpy(out->iris, body + 0xA8, 8);
        }
    }
    free(raw);
    return rc;
}

int roomstate_init(void) {
    BSDR_DEBUG("bsdr.roomstate", "roomstate: FlatBuffers codec ready");
    return 0;
}
