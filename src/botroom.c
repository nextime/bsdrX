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
/* Full-bot avatar presence — see bsdr/botroom.h. */
#include "bsdr/botroom.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BSDR_ENABLE_SCTP
#include "bsdr/udp_transport.h"
#include "bsdr/sctp.h"
#include "bsdr/roomstate.h"

struct bsdr_botroom {
    bsdr_thread *th;
    volatile int stop;
    volatile int avatar_state;   /* bsdr_avatar_state — live, polled by the UI */
    char ip[64];
    int  data_port;
    char user_id[160];   /* the room legacyUserId ("userNNN") — data-channel prefix + UserState.userId */
    int  seat;
};

/* Inbound room data (other peers' avatar/identity FlatBuffers). We only need to broadcast to be
 * visible, but decoding peers is cheap and gives a useful "N peers seen" signal in the log. */
static void botroom_rx(const uint8_t *data, size_t len, void *user) {
    (void)user;
    rs_decoded d;
    if (roomstate_decode(data, len, &d) < 0) return;
    if (d.type == RS_TYPE_USER_STATE && d.user_id[0])
        BSDR_DEBUG("bsdr.botroom", "peer UserState uid=%s seat=%d", d.user_id, d.seat_index);
}

/* Bring up the raw SCTP association to the relay (no DCEP), retrying through ABORTs exactly like the
 * host's receive-only path in cloud_stream.c. Returns an associated sctp (caller frees) or NULL. */
static bsdr_sctp *associate(struct bsdr_botroom *b, bsdr_udp *udp) {
    uint8_t buf[2048];
    for (int attempt = 0; attempt < 5 && !b->stop; attempt++) {
        bsdr_sctp *sctp = bsdr_sctp_new_udp(udp, true /*initiator*/, botroom_rx, b);
        if (!sctp || !bsdr_sctp_start(sctp, 5000)) { if (sctp) bsdr_sctp_free(sctp); return NULL; }
        BSDR_INFO("bsdr.botroom", "avatar: raw SCTP INIT -> relay %s:%d%s",
                  b->ip, b->data_port, attempt ? " [retry]" : "");
        uint64_t last = bsdr_now_ms(), t0 = last;
        while (!b->stop) {
            int n = bsdr_udp_recv(udp, buf, sizeof buf, 50);
            if (n > 0) bsdr_sctp_feed(sctp, buf, (size_t)n);
            uint64_t now = bsdr_now_ms();
            bsdr_sctp_handle_timers((uint32_t)(now - last)); last = now;
            if (bsdr_sctp_associated(sctp)) return sctp;
            if (bsdr_sctp_failed(sctp)) break;      /* ABORT -> recreate + retry */
            if (now - t0 > 4000) break;             /* timeout -> retry */
        }
        bsdr_sctp_free(sctp);
        if (!b->stop) bsdr_sleep_ms(300);
    }
    return NULL;
}

static void botroom_thread(void *arg) {
    struct bsdr_botroom *b = (struct bsdr_botroom *)arg;
    bsdr_udp udp;
    /* Ephemeral local port (comedia: the relay learns our address from our first packet). */
    if (!bsdr_udp_open(&udp, 0, b->ip, (uint16_t)b->data_port)) {
        BSDR_WARN("bsdr.botroom", "avatar: udp -> %s:%d failed", b->ip, b->data_port);
        return;
    }
    bsdr_sctp *sctp = associate(b, &udp);
    if (!sctp) {
        b->avatar_state = BSDR_AVATAR_GHOST;
        BSDR_WARN("bsdr.botroom", "avatar: SCTP did not associate; the bot stays a userlist ghost");
        bsdr_udp_close(&udp);
        return;
    }
    b->avatar_state = BSDR_AVATAR_UP;
    BSDR_INFO("bsdr.botroom", "avatar: associated; broadcasting UserState + pose for %s", b->user_id);

    /* 1) UserState once — identity + seat, wearing_hmd/showing_head so the avatar renders. */
    char *msg = NULL; size_t mlen = 0;
    if (roomstate_user_state(&msg, &mlen, b->user_id, /*area*/0, b->seat) == 0) {
        bsdr_sctp_send_room(sctp, (const uint8_t *)msg, mlen);
        free(msg);
    }

    /* 2) TickState at 10 Hz — a static standing head pose facing the screen (identity rotation, eye
     * height). Re-send UserState every ~2 s so late joiners learn who we are. */
    rs_pose head; memset(&head, 0, sizeof head);
    head.pos[1] = 1.6f;      /* eye height (m) */
    head.rot[3] = 1.0f;      /* identity quaternion */
    uint8_t buf[2048];
    uint64_t last = bsdr_now_ms(), last_us = last;
    while (!b->stop) {
        /* keep the association live + drain inbound */
        int n = bsdr_udp_recv(&udp, buf, sizeof buf, 20);
        if (n > 0) bsdr_sctp_feed(sctp, buf, (size_t)n);
        uint64_t now = bsdr_now_ms();
        bsdr_sctp_handle_timers((uint32_t)(now - last)); last = now;
        if (bsdr_sctp_failed(sctp)) {                 /* lost the assoc -> reassociate */
            BSDR_WARN("bsdr.botroom", "avatar: association lost; reconnecting");
            b->avatar_state = BSDR_AVATAR_CONNECTING;
            bsdr_sctp_free(sctp);
            sctp = associate(b, &udp);
            if (!sctp) { b->avatar_state = BSDR_AVATAR_GHOST; break; }
            b->avatar_state = BSDR_AVATAR_UP;
            if (roomstate_user_state(&msg, &mlen, b->user_id, 0, b->seat) == 0) {
                bsdr_sctp_send_room(sctp, (const uint8_t *)msg, mlen); free(msg);
            }
            last = bsdr_now_ms(); last_us = last;
            continue;
        }
        char *tk = NULL; size_t tl = 0;
        if (roomstate_tick_state(&tk, &tl, b->user_id, /*mic_loudness*/0.0f, &head) == 0) {
            bsdr_sctp_send_room(sctp, (const uint8_t *)tk, tl);
            free(tk);
        }
        if (now - last_us >= 2000) {                  /* periodic UserState refresh */
            if (roomstate_user_state(&msg, &mlen, b->user_id, 0, b->seat) == 0) {
                bsdr_sctp_send_room(sctp, (const uint8_t *)msg, mlen); free(msg);
            }
            last_us = now;
        }
        bsdr_sleep_ms(100);
    }
    if (sctp) bsdr_sctp_free(sctp);
    bsdr_udp_close(&udp);
    BSDR_INFO("bsdr.botroom", "avatar: presence stopped");
}

bsdr_botroom *bsdr_botroom_start(const char *relay_ip, int data_port,
                                 const char *legacy_user_id, int seat_index) {
    if (!relay_ip || !relay_ip[0] || data_port <= 0) return NULL;
    struct bsdr_botroom *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    snprintf(b->ip, sizeof b->ip, "%s", relay_ip);
    b->data_port = data_port;
    snprintf(b->user_id, sizeof b->user_id, "%s", legacy_user_id ? legacy_user_id : "");
    b->seat = seat_index;
    b->avatar_state = BSDR_AVATAR_CONNECTING;   /* until the thread associates or gives up */
    b->th = bsdr_thread_start(botroom_thread, b);
    if (!b->th) { free(b); return NULL; }
    return b;
}

bsdr_avatar_state bsdr_botroom_avatar_state(const bsdr_botroom *b) {
    return b ? (bsdr_avatar_state)b->avatar_state : BSDR_AVATAR_OFF;
}

void bsdr_botroom_stop(bsdr_botroom *b) {
    if (!b) return;
    b->stop = 1;
    if (b->th) bsdr_thread_join(b->th);
    free(b);
}

#else  /* !BSDR_ENABLE_SCTP — no media/SCTP stack: full-bot avatar unavailable, stay audio-only. */

bsdr_botroom *bsdr_botroom_start(const char *relay_ip, int data_port,
                                 const char *user_id, int seat_index) {
    (void)relay_ip; (void)data_port; (void)user_id; (void)seat_index;
    BSDR_WARN("bsdr.botroom", "full-bot avatar needs the SCTP media build; staying audio-only");
    return NULL;
}
void bsdr_botroom_stop(bsdr_botroom *b) { (void)b; }
bsdr_avatar_state bsdr_botroom_avatar_state(const bsdr_botroom *b) { (void)b; return BSDR_AVATAR_OFF; }

#endif
