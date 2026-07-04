/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log

/**
 * Voice computer control input: the DEVICE's own microphone (Android has no
 * owner-mic sniffer — that needs root). Captures 48 kHz mono int16 (matching the
 * voice pipeline's rate) from the VOICE_RECOGNITION source and pushes it into the
 * native voice VAD via NativeBridge.nativePushVoiceMic. Runs only while computer
 * control is armed; the native side gates the actual listen cycle on a bubble tap.
 */
class VoiceMic {
    private val rate = 48000
    private var record: AudioRecord? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    @SuppressLint("MissingPermission")   // RECORD_AUDIO is granted before the service starts
    fun start() {
        if (running) return
        val min = AudioRecord.getMinBufferSize(rate,
            AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        record = try {
            AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, rate,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(min, rate))
        } catch (e: Exception) { Log.w(TAG, "mic open", e); null }
        val r = record ?: return
        if (r.state != AudioRecord.STATE_INITIALIZED) { Log.w(TAG, "mic uninitialized"); r.release(); record = null; return }
        running = true
        r.startRecording()
        thread = Thread({ loop() }, "bsdr-voicemic").also { it.start() }
    }

    private fun loop() {
        val buf = ShortArray(480)          // 10 ms @ 48 kHz mono
        val r = record ?: return
        while (running) {
            val n = r.read(buf, 0, buf.size)
            if (n > 0) NativeBridge.nativePushVoiceMic(buf, n)
        }
    }

    fun stop() {
        running = false
        try { thread?.join(300) } catch (_: InterruptedException) {}
        try { record?.stop(); record?.release() } catch (e: Exception) { Log.w(TAG, "mic stop", e) }
        record = null; thread = null
    }

    companion object { private const val TAG = "bsdr.voicemic" }
}
