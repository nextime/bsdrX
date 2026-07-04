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
/* Decode + resample the audio track of a media file to interleaved S16 PCM, paced to its
 * presentation time so it plays at real speed and stays in sync with the paced video
 * (bsdr_capture file mode). One instance = one consumer; open two on the same file to feed
 * both the LAN Quest and the cloud room, mirroring how the desktop path uses two captures. */
#ifndef BSDR_FILEAUDIO_H
#define BSDR_FILEAUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct bsdr_fileaudio bsdr_fileaudio;

/* Open `path`'s audio track, resampled to out_rate / out_channels S16 interleaved. Returns NULL
 * if the file has no audio track or on error. `loop` seeks to 0 at EOF (match the video loop). */
bsdr_fileaudio *bsdr_fileaudio_open(const char *path, int out_rate, int out_channels, bool loop);

/* Fill `pcm` with exactly `frames` samples/channel of paced PCM. Returns `frames` on success,
 * 0 if none are ready yet (caller retries), <0 at EOF (non-loop). Blocks up to ~a frame while
 * pacing to presentation time. `pcm` must hold frames*out_channels int16 samples. */
int bsdr_fileaudio_read(bsdr_fileaudio *fa, int16_t *pcm, int frames);

void bsdr_fileaudio_seek(bsdr_fileaudio *fa, double frac);
void bsdr_fileaudio_set_paused(bsdr_fileaudio *fa, int paused);
void bsdr_fileaudio_close(bsdr_fileaudio *fa);

#endif /* BSDR_FILEAUDIO_H */
