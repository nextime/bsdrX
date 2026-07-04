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
/* Verify the PulseAudio capture path + virtual-device setup against the running
 * sound server. Non-destructive: it restores the previous default sink.
 *   ./audio_test
 */
#include "bsdr/audio.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    bsdr_log_set_level(BSDR_LOG_INFO);
    int fail = 0;

    /* 1) create the virtual devices (null-sink speaker + virtual mic) */
    bsdr_audio_devices dev;
    if (!bsdr_audio_devices_create(&dev)) {
        printf("FAIL device_create (no PulseAudio?)\n");
        return 1;
    }
    printf("PASS device_create (monitor=%s mic_sink=%s)\n", dev.monitor_source, dev.mic_sink);

    /* 2) capture ~200 ms of desktop audio from the speaker monitor */
    bsdr_pa *rec = bsdr_pa_record_open(dev.monitor_source, 2);
    if (rec) {
        int16_t pcm[BSDR_OPUS_FRAME * 2];
        int frames = 0;
        for (int i = 0; i < 10; i++)
            if (bsdr_pa_read(rec, pcm, BSDR_OPUS_FRAME) == BSDR_OPUS_FRAME) frames += BSDR_OPUS_FRAME;
        bsdr_pa_close(rec);
        if (frames >= BSDR_OPUS_FRAME * 9) printf("PASS pa_capture (%d samples from monitor)\n", frames);
        else { printf("FAIL pa_capture (%d samples)\n", frames); fail++; }
    } else { printf("FAIL pa_record_open\n"); fail++; }

    /* 3) play 200 ms of silence into the virtual mic sink */
    bsdr_pa *play = bsdr_pa_play_open(dev.mic_sink, 2);
    if (play) {
        int16_t pcm[BSDR_OPUS_FRAME * 2];
        memset(pcm, 0, sizeof(pcm));
        int ok = 1;
        for (int i = 0; i < 10; i++) if (bsdr_pa_write(play, pcm, BSDR_OPUS_FRAME) != BSDR_OPUS_FRAME) ok = 0;
        bsdr_pa_close(play);
        printf(ok ? "PASS pa_playback (to virtual mic sink)\n" : "FAIL pa_playback\n");
        fail += !ok;
    } else { printf("FAIL pa_play_open\n"); fail++; }

    /* 4) tear down (restores the previous default sink) */
    bsdr_audio_devices_destroy(&dev);
    printf("PASS device_destroy (default sink restored)\n");

    printf(fail ? "\nFAILED (%d)\n" : "\nOK - PulseAudio capture/playback + virtual devices work\n", fail);
    return fail ? 1 : 0;
}
