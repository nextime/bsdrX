/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import androidx.annotation.Keep

/**
 * The single seam to the native agent (libbsdr_agent.so). Kotlin -> native calls
 * push encoded frames / PCM and control the lifecycle; native -> Kotlin callbacks
 * (whose names match the method IDs cached in bsdr_jni.c) fan out to listeners
 * set by the service, the AccessibilityService and the audio path.
 */
object NativeBridge {

    /** Local control-UI port the native web server listens on (loopback). */
    const val UI_PORT = 8088

    /** Input-command kinds, mirroring bsdr_android_input_kind in bsdr_android.h. */
    object Input {
        const val MOVE = 1; const val TAP = 2; const val DOWN = 3; const val UP = 4
        const val SCROLL = 5; const val TEXT = 6; const val KEY = 7
    }

    init { System.loadLibrary("bsdr_agent") }

    // ---- Kotlin -> native ----------------------------------------------------
    private external fun nativeStart(w: Int, h: Int, fps: Int, bitrate: Int, uiPort: Int)
    private external fun nativeStop()
    external fun nativePushVideo(buf: ByteArray, len: Int, ptsUs: Long, isConfig: Boolean)
    external fun nativePushAudio(pcm: ShortArray, frames: Int, channels: Int)
    /** Fills out[w,h,fps,bitrate] and returns true when the headset changed quality. */
    external fun nativePollVideoWant(out: IntArray): Boolean
    /** Poll 2D->3D config: out = [mode, deepness, convergence, swap, full]. True if changed. */
    external fun nativePollThreed(out: IntArray): Boolean

    // ---- voice computer control (device mic) --------------------------------
    /** Mono int16 device-mic PCM into the voice VAD (fed while compctl is armed). */
    external fun nativePushVoiceMic(pcm: ShortArray, frames: Int)
    /** Bubble tap: start a listen cycle / stop listening / Send(true)|Cancel(false). */
    external fun nativeVoiceTrigger()
    external fun nativeVoiceStop()
    external fun nativeVoiceConfirm(send: Boolean)

    /** Voice pipeline state, mirroring bsdr_voice_state in voice.h. */
    object Voice { const val IDLE = 0; const val LISTENING = 1; const val CONFIRM = 2; const val WORKING = 3 }

    // ---- native -> Kotlin listeners -----------------------------------------
    @Volatile var pairingListener: ((String) -> Unit)? = null
    @Volatile var micListener: ((ShortArray, Int, Int) -> Unit)? = null
    @Volatile var inputListener: ((Int, Int, Int) -> Unit)? = null
    @Volatile var voiceStateListener: ((Int) -> Unit)? = null
    @Volatile var voiceFeedbackListener: ((String) -> Unit)? = null
    @Volatile var compctlActiveListener: ((Boolean) -> Unit)? = null
    /** Grab a JPEG of the screen (long side <= maxDim) for the vision model. Called
     *  synchronously from the native voice worker; provider must block until ready. */
    @Volatile var screenshotProvider: ((Int) -> ByteArray?)? = null

    @Volatile var running: Boolean = false
        private set

    fun start(width: Int, height: Int, fps: Int, bitrate: Int) {
        if (running) return
        running = true
        nativeStart(width, height, fps, bitrate, UI_PORT)
    }

    fun stop() {
        if (!running) return
        running = false
        nativeStop()
    }

    // ---- callbacks invoked FROM native (do not rename) ----------------------
    @Keep fun onPairingCode(code: String) { pairingListener?.invoke(code) }
    @Keep fun onMicPcm(pcm: ShortArray, frames: Int, channels: Int) =
        micListener?.invoke(pcm, frames, channels) ?: Unit
    @Keep fun onInputEvent(kind: Int, a: Int, b: Int) { inputListener?.invoke(kind, a, b) }
    @Keep fun onVoiceState(state: Int) { voiceStateListener?.invoke(state) }
    @Keep fun onVoiceFeedback(msg: String) { voiceFeedbackListener?.invoke(msg) }
    @Keep fun onCompctlActive(active: Boolean) { compctlActiveListener?.invoke(active) }
    @Keep fun grabScreenshot(maxDim: Int): ByteArray? = screenshotProvider?.invoke(maxDim)
}
