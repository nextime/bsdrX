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
/* Local video-file source: demux an H.264 file and emit its NAL units directly
 * (no re-encode) as Annex-B access units for the RTP/SRTP sender. MP4/MOV AVCC
 * is converted to Annex-B via the h264_mp4toannexb bitstream filter; raw/TS
 * streams pass through. Paced to the file's timestamps. BSDR_ENABLE_VIDEO+Linux. */
#ifndef BSDR_FILESRC_H
#define BSDR_FILESRC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct bsdr_filesrc bsdr_filesrc;

/* `loop` restarts at EOF for continuous streaming. */
bsdr_filesrc *bsdr_filesrc_open(const char *path, bool loop);

/* Next access unit. Sets *au (valid until the next call), *len, *rtp_ts (90 kHz).
 * Blocks for pacing. Returns 1 = frame, 0 = skip/again, <0 = EOF (no loop). */
int bsdr_filesrc_frame(bsdr_filesrc *f, const uint8_t **au, size_t *len,
                       uint32_t *rtp_ts);

void bsdr_filesrc_info(bsdr_filesrc *f, int *w, int *h, double *fps);
/* Seek to a fraction (0..1) of the file duration. */
void bsdr_filesrc_seek(bsdr_filesrc *f, double frac);
void bsdr_filesrc_close(bsdr_filesrc *f);

#endif /* BSDR_FILESRC_H */
