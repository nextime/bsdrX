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
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.projection.MediaProjection
import android.util.Log

/**
 * Desktop-out: AudioPlaybackCapture (system audio, MediaProjection-scoped, API 29+)
 * -> int16 PCM -> NativeBridge.nativePushAudio. Mic-in: native decodes the Quest
 * mic and calls NativeBridge.micListener -> AudioTrack (Android has no virtual mic
 * without root, so it plays out / can feed the assistant). 48 kHz, stereo out /
 * mono in, fixed by the protocol.
 */
class AudioIO(private val projection: MediaProjection) {
    private val rate = 48000
    private var record: AudioRecord? = null
    private var track: AudioTrack? = null
    private var roomTrack: AudioTrack? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    @SuppressLint("MissingPermission") // RECORD_AUDIO is requested before the service starts
    fun start() {
        if (running) return
        running = true

        val config = AudioPlaybackCaptureConfiguration.Builder(projection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()
        val inFormat = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(rate)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        val minIn = AudioRecord.getMinBufferSize(rate,
            AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        record = AudioRecord.Builder()
            .setAudioPlaybackCaptureConfig(config)
            .setAudioFormat(inFormat)
            .setBufferSizeInBytes(maxOf(minIn, rate))   // ~0.5 s headroom
            .build()

        // mic-out: decoded Quest mic -> speaker/headphone
        val minOut = AudioTrack.getMinBufferSize(rate,
            AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(maxOf(minOut, rate))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build().also { it.play() }

        NativeBridge.micListener = { pcm, frames, _ -> track?.write(pcm, 0, frames) }

        // room mic (other participants' voices from the cloud SFU): a SEPARATE track with
        // USAGE_MEDIA so it's grabbable by AudioPlaybackCapture ("internal audio" recorders) —
        // Android's closest thing to the desktop BSDR_RoomMic virtual mic (no user-space virtual
        // input device exists). Distinct from the VOICE_COMMUNICATION mic-out above.
        roomTrack = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(maxOf(minOut, rate))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build().also { it.play() }
        NativeBridge.roomListener = { pcm, frames, _ -> roomTrack?.write(pcm, 0, frames) }

        record?.startRecording()
        thread = Thread({ captureLoop() }, "bsdr-audio").also { it.start() }
    }

    private fun captureLoop() {
        val frame = 480                      // 10 ms @ 48 kHz, matches the encoder
        val buf = ShortArray(frame * 2)      // stereo interleaved
        val r = record ?: return
        while (running) {
            val n = r.read(buf, 0, buf.size)
            if (n <= 0) continue
            NativeBridge.nativePushAudio(buf, n / 2, 2)
        }
    }

    fun stop() {
        running = false
        NativeBridge.micListener = null
        NativeBridge.roomListener = null
        try { thread?.join(300) } catch (_: InterruptedException) {}
        try { record?.stop(); record?.release() } catch (e: Exception) { Log.w(TAG, "record stop", e) }
        try { track?.stop(); track?.release() } catch (e: Exception) { Log.w(TAG, "track stop", e) }
        try { roomTrack?.stop(); roomTrack?.release() } catch (e: Exception) { Log.w(TAG, "roomTrack stop", e) }
        record = null; track = null; roomTrack = null
    }

    companion object { private const val TAG = "bsdr.audio" }
}
