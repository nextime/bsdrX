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
/* Overlay rendering + click hit-testing (no GPU). */
#include "bsdr/overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 180

int main(void) {
    int fail = 0;
    uint8_t *y = malloc(W * H);
    uint8_t *uv = malloc(W * H / 2);
    memset(y, 128, W * H);
    memset(uv, 128, W * H / 2);

    bsdr_overlay *o = bsdr_overlay_new();
    bsdr_overlay_set_playing(o, false);   /* show the play triangle */
    bsdr_overlay_set_volume(o, 50);
    bsdr_overlay_set_position(o, 0.3, true);
    bsdr_overlay_render_nv12(o, y, W, uv, W, W, H);

    /* the bar background (bottom strip) should be darkened, and icons bright */
    int dark = 0, bright = 0;
    for (int r = (int)(0.86 * H); r < H; r++)
        for (int c = 0; c < W; c++) {
            uint8_t v = y[r * W + c];
            if (v < 100) dark++;
            if (v > 200) bright++;
        }
    if (dark > 200) printf("PASS bar_darkened (%d px)\n", dark);
    else { printf("FAIL bar_darkened (%d)\n", dark); fail++; }
    if (bright > 50) printf("PASS icons_rendered (%d bright px)\n", bright);
    else { printf("FAIL icons_rendered (%d)\n", bright); fail++; }
    /* above the bar must be untouched */
    if (y[(int)(0.5 * H) * W + W / 2] == 128) printf("PASS content_preserved\n");
    else { printf("FAIL content_preserved\n"); fail++; }

    /* hit-testing */
    double val;
    if (bsdr_overlay_hit(o, 0.05, 0.93, &val) == BSDR_OVL_PLAYPAUSE) printf("PASS hit_playpause\n");
    else { printf("FAIL hit_playpause\n"); fail++; }
    if (bsdr_overlay_hit(o, 0.96, 0.93, &val) == BSDR_OVL_EXIT) printf("PASS hit_exit\n");
    else { printf("FAIL hit_exit\n"); fail++; }
    if (bsdr_overlay_hit(o, 0.105, 0.93, &val) == BSDR_OVL_VOICE) printf("PASS hit_voice\n");
    else { printf("FAIL hit_voice\n"); fail++; }
    bsdr_overlay_action a = bsdr_overlay_hit(o, (0.15 + 0.62) / 2, 0.93, &val);
    if (a == BSDR_OVL_SEEK && val > 0.45 && val < 0.55) printf("PASS hit_seek (%.2f)\n", val);
    else { printf("FAIL hit_seek (a=%d val=%.2f)\n", a, val); fail++; }
    if (bsdr_overlay_hit(o, 0.66, 0.93, &val) == BSDR_OVL_VOL_DOWN) printf("PASS hit_vol_down\n");
    else { printf("FAIL hit_vol_down\n"); fail++; }
    if (bsdr_overlay_hit(o, 0.5, 0.5, &val) == BSDR_OVL_NONE) printf("PASS miss_above_bar\n");
    else { printf("FAIL miss_above_bar\n"); fail++; }

    /* ---- voice-command balloon (packed BGRA source frame) ---- */
    uint8_t *bgra = calloc((size_t)W * H, 4);
    /* off by default: render is a no-op, hit-test always misses */
    bsdr_overlay_render_balloon(o, bgra, W * 4, W, H, 1, 1000);
    if (!bsdr_overlay_balloon_on(o) && !bsdr_overlay_balloon_hit(o, 0.5, 0.12))
        printf("PASS balloon_off_default\n");
    else { printf("FAIL balloon_off_default\n"); fail++; }

    bsdr_overlay_set_balloon(o, true);
    bsdr_overlay_set_balloon_pos(o, 0.5, 0.25);
    bsdr_overlay_render_balloon(o, bgra, W * 4, W, H, 1, 1000);
    int cxp = (int)(0.5 * W), cyp = (int)(0.25 * H);
    uint8_t *cpx = bgra + ((size_t)cyp * W + cxp) * 4;
    if (cpx[0] || cpx[1] || cpx[2]) printf("PASS balloon_drawn_at_center\n");
    else { printf("FAIL balloon_drawn_at_center\n"); fail++; }
    if (bgra[0] == 0 && bgra[1] == 0 && bgra[2] == 0) printf("PASS balloon_corner_clean\n");
    else { printf("FAIL balloon_corner_clean\n"); fail++; }
    if (bsdr_overlay_balloon_hit(o, 0.5, 0.25)) printf("PASS balloon_hit_center\n");
    else { printf("FAIL balloon_hit_center\n"); fail++; }
    if (!bsdr_overlay_balloon_hit(o, 0.9, 0.8)) printf("PASS balloon_miss_far\n");
    else { printf("FAIL balloon_miss_far\n"); fail++; }
    /* position is clamped so the balloon can't leave the frame */
    bsdr_overlay_set_balloon_pos(o, 2.0, -1.0);
    double bx, by; bsdr_overlay_get_balloon_pos(o, &bx, &by);
    if (bx <= 1.0 && bx > 0.5 && by >= 0.0 && by < 0.5) printf("PASS balloon_pos_clamped\n");
    else { printf("FAIL balloon_pos_clamped (%.2f,%.2f)\n", bx, by); fail++; }
    /* listening flips the body color (red channel dominant vs cyan) */
    memset(bgra, 0, (size_t)W * H * 4);
    bsdr_overlay_set_balloon_pos(o, 0.5, 0.25);
    bsdr_overlay_set_listening(o, true);
    bsdr_overlay_render_balloon(o, bgra, W * 4, W, H, 1, 1000);
    /* sample a body pixel off-axis (avoid the white mic glyph on the center column). Must stay
     * inside BALLOON_RX (0.025) and outside the mic glyph (rx/3 ≈ 0.008). */
    uint8_t *bp = bgra + ((size_t)cyp * W + (cxp + (int)(0.015 * W))) * 4;
    if (bp[2] > bp[0]) printf("PASS balloon_listening_red\n");   /* R > B in BGRA */
    else { printf("FAIL balloon_listening_red (B=%d G=%d R=%d)\n", bp[0], bp[1], bp[2]); fail++; }
    bsdr_overlay_set_listening(o, false);

    /* Send / Cancel confirm row */
    bsdr_overlay_set_balloon_pos(o, 0.5, 0.3);
    bsdr_overlay_get_balloon_pos(o, &bx, &by);
    /* Tap points track the overlay geometry (BTN_DY=BALLOON_RY+0.0375, BTN_OFF=0.0475,
     * STOP_DX=0.06, HANDLE_DY=BALLOON_RY+0.0225) — update together if the balloon is resized. */
    if (bsdr_overlay_confirm_hit(o, bx, by + 0.0725) == 0) printf("PASS confirm_off_when_not_shown\n");
    else { printf("FAIL confirm_off_when_not_shown\n"); fail++; }
    bsdr_overlay_set_confirm(o, true);
    if (bsdr_overlay_confirm_hit(o, bx - 0.0475, by + 0.0725) == 1) printf("PASS confirm_send\n");
    else { printf("FAIL confirm_send\n"); fail++; }
    if (bsdr_overlay_confirm_hit(o, bx + 0.0475, by + 0.0725) == 2) printf("PASS confirm_cancel\n");
    else { printf("FAIL confirm_cancel\n"); fail++; }
    bsdr_overlay_set_confirm(o, false);

    /* stop balloon (working) */
    if (!bsdr_overlay_stop_hit(o, bx + 0.06, by)) printf("PASS stop_off_when_idle\n");
    else { printf("FAIL stop_off_when_idle\n"); fail++; }
    bsdr_overlay_set_working(o, true);
    double sx = bx < 0.5 ? bx + 0.06 : bx - 0.06;
    if (bsdr_overlay_stop_hit(o, sx, by)) printf("PASS stop_hit\n");
    else { printf("FAIL stop_hit\n"); fail++; }
    bsdr_overlay_set_working(o, false);

    /* history handle + feedback text drawn */
    if (bsdr_overlay_history_hit(o, bx, by + 0.0575)) printf("PASS history_handle_hit\n");
    else { printf("FAIL history_handle_hit\n"); fail++; }
    bsdr_overlay_push_feedback(o, "hello world");
    bsdr_overlay_toggle_history(o);
    memset(bgra, 0, (size_t)W * H * 4);
    bsdr_overlay_render_balloon(o, bgra, W * 4, W, H, 1, 2000);
    long lit = 0;
    for (long i = 0; i < (long)W * H; i++) if (bgra[i*4] || bgra[i*4+1] || bgra[i*4+2]) lit++;
    if (lit > 200) printf("PASS history_panel_drawn (%ld px)\n", lit);
    else { printf("FAIL history_panel_drawn (%ld)\n", lit); fail++; }

    free(bgra);
    bsdr_overlay_free(o);
    free(y); free(uv);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - overlay render + hit-test passed\n", fail);
    return fail ? 1 : 0;
}
