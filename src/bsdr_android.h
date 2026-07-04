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
/* Android-only glue between the platform shims (capture/audio/inject) and the
 * JNI bridge (bsdr_jni.c). The shims implement the portable bsdr_* headers; this
 * header is the thin extra seam the JNI bridge uses to feed encoded frames /
 * PCM into the agent and to receive decoded mic PCM + input commands back out.
 *
 * Encoding/decoding and the OS APIs live in Kotlin (MediaCodec / AudioRecord /
 * AudioTrack / AccessibilityService); the C core stays the single source of
 * truth for the wire protocol. */
#ifndef BSDR_ANDROID_H
#define BSDR_ANDROID_H

#include <stdint.h>
#include <stddef.h>

/* One-time init for the shim globals (mutexes). Call from the JNI start path
 * before the agent thread is spawned. Idempotent. */
void bsdr_android_capture_init(void);
void bsdr_android_audio_init(void);

/* ---- video: Kotlin MediaCodec -> capture_android ring buffer --------------*/
/* Feed one encoded H.264 access unit (Annex-B) to the currently-open Android
 * capture. is_config=1 marks a codec-config buffer (SPS/PPS) which is cached
 * and prepended to the next IDR (the Quest expects in-band SPS/PPS per IDR). */
void bsdr_android_push_video(const uint8_t *au, size_t len, int64_t pts_us, int is_config);

/* capture_android publishes the headset-requested stream params (bitrate live,
 * resolution on a codec recreate). The JNI bridge polls this and drives
 * MediaCodec.setParameters / a codec recreate. Returns 1 if changed since the
 * last poll, filling the out params; 0 otherwise. */
int  bsdr_android_capture_want(int *width, int *height, int *fps, int *bitrate);

/* 2D->3D: the agent publishes the current SBS config here; the JNI bridge polls it and drives the
 * Kotlin GL pipeline (MediaProjection -> SBS shader -> MediaCodec). mode 0=off/1=fast/2=ai (ai is
 * treated as fast on Android). poll returns 1 if changed since the last poll. */
void bsdr_android_publish_threed(int mode, int deepness, int convergence, int swap, int full);
int  bsdr_android_poll_threed(int *mode, int *deepness, int *convergence, int *swap, int *full);

/* ---- audio: Kotlin AudioRecord -> audio_android capture -------------------*/
/* Interleaved int16 system-audio PCM (AudioPlaybackCapture) into the agent. */
void bsdr_android_push_audio(const int16_t *pcm, int frames, int channels);

/* audio_android -> Kotlin AudioTrack: decoded Quest-mic PCM out. Implemented in
 * bsdr_jni.c (calls the NativeBridge mic callback). */
void bsdr_android_emit_mic(const int16_t *pcm, int frames, int channels);

/* ---- input: inject_android -> Kotlin AccessibilityService -----------------*/
typedef enum {
    BSDR_AINPUT_MOVE = 1,   /* a,b = cursor x,y px */
    BSDR_AINPUT_TAP,        /* a,b = x,y px */
    BSDR_AINPUT_DOWN,       /* a,b = gesture-start x,y px */
    BSDR_AINPUT_UP,         /* a,b = gesture-end x,y px */
    BSDR_AINPUT_SCROLL,     /* a,b = dx,dy notches */
    BSDR_AINPUT_TEXT,       /* a = unicode codepoint */
    BSDR_AINPUT_KEY         /* a = Android keycode (nav keys) */
} bsdr_android_input_kind;

/* Marshalled, screen-mapped input command for the AccessibilityService.
 * Implemented in bsdr_jni.c. */
void bsdr_android_emit_input(int kind, int a, int b);

/* ---- voice computer control (device mic -> voice pipeline) ----------------*/
/* On Android there is no owner-mic sniffer (no root); the DEVICE's own mic is the
 * voice-control source. The agent registers its live voice object here when
 * computer control is armed (NULL to disarm); the JNI bridge feeds device-mic PCM
 * and routes the bubble's tap/confirm gestures to it. Implemented in
 * voice_android.c. All are no-ops until a voice object is registered. */
struct bsdr_voice;
void bsdr_android_set_voice(struct bsdr_voice *v);      /* agent -> bridge (arm/disarm) */
void bsdr_android_push_voice(const int16_t *pcm, int frames);   /* Kotlin mic -> voice VAD */
void bsdr_android_voice_trigger(void);                  /* bubble tap: start a listen cycle */
void bsdr_android_voice_stop(void);                     /* bubble tap while listening: stop */
void bsdr_android_voice_confirm(int send);              /* Send (1) / Cancel (0) the message */

/* voice_android -> Kotlin: state (bsdr_voice_state) + one-line feedback for the
 * bubble UI, and armed/disarmed so Kotlin starts/stops the mic. In bsdr_jni.c. */
void bsdr_android_emit_voice_state(int state);
void bsdr_android_emit_voice_feedback(const char *msg);
void bsdr_android_emit_compctl_active(int active);

/* ---- vision: on-demand screenshot (voice model tool) ----------------------*/
/* Grab one frame of the device screen as JPEG (long side <= max_dim) into out,
 * returning the JPEG byte count (0 on failure / no cap). Blocks the caller (the
 * voice worker) until Kotlin captures + encodes via MediaProjection. In bsdr_jni.c. */
int bsdr_android_screenshot(int max_dim, uint8_t *out, size_t cap);

#endif /* BSDR_ANDROID_H */
