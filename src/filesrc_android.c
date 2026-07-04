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
/* Android file source: stub. File playback is an ffmpeg-demux feature on the
 * desktop; on Android v1 the agent casts the live screen (MediaProjection ->
 * MediaCodec) only, so opening a file source fails and transport.c falls back to
 * live capture. Lets transport.c link without libavformat on Android. */
#include "bsdr/filesrc.h"
#include "bsdr/fileaudio.h"
#include "bsdr/log.h"

bsdr_filesrc *bsdr_filesrc_open(const char *path, bool loop) {
    (void)path; (void)loop;
    BSDR_WARN("bsdr.filesrc", "file playback unsupported on Android (live screen only)");
    return NULL;
}
int  bsdr_filesrc_frame(bsdr_filesrc *f, const uint8_t **au, size_t *len, uint32_t *rtp_ts) {
    (void)f; (void)au; (void)len; (void)rtp_ts; return -1;
}
void bsdr_filesrc_info(bsdr_filesrc *f, int *w, int *h, double *fps) {
    (void)f; if (w) *w = 0; if (h) *h = 0; if (fps) *fps = 0;
}
void bsdr_filesrc_seek(bsdr_filesrc *f, double frac) { (void)f; (void)frac; }
void bsdr_filesrc_close(bsdr_filesrc *f) { (void)f; }

/* File AUDIO is an ffmpeg-decode feature too (fileaudio.c), which the Android build omits. Stub the
 * API so agent.c / cloud_stream.c link; open returns NULL so callers stream no file audio (matching
 * the video-source stub above — Android casts the live screen, not local files). */
bsdr_fileaudio *bsdr_fileaudio_open(const char *path, int out_rate, int out_channels, bool loop) {
    (void)path; (void)out_rate; (void)out_channels; (void)loop; return NULL;
}
int  bsdr_fileaudio_read(bsdr_fileaudio *fa, int16_t *pcm, int frames) {
    (void)fa; (void)pcm; (void)frames; return -1;
}
void bsdr_fileaudio_seek(bsdr_fileaudio *fa, double frac) { (void)fa; (void)frac; }
void bsdr_fileaudio_set_paused(bsdr_fileaudio *fa, int paused) { (void)fa; (void)paused; }
void bsdr_fileaudio_close(bsdr_fileaudio *fa) { (void)fa; }
