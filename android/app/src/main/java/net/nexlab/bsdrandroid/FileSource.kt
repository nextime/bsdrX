/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.Surface

/**
 * Stream a local video FILE to the headset (the Android analogue of the desktop
 * `--file` source). Instead of MediaProjection, a MediaExtractor + MediaCodec
 * decoder renders the file to a MediaCodec AVC encoder's input surface (Constrained
 * Baseline, to match the Quest) — a transcode, so any input codec/profile works —
 * and the encoded Annex-B access units go out through the same NativeBridge.
 * nativePushVideo path as the live encoder. Loops at end-of-stream; paced to the
 * file's presentation timestamps.
 */
class FileSource(
    private val ctx: Context,
    private val uri: Uri,
    private var width: Int,
    private var height: Int,
    private var fps: Int,
    private var bitrate: Int,
) {
    private var extractor: MediaExtractor? = null
    private var decoder: MediaCodec? = null
    private var encoder: MediaCodec? = null
    private var surface: Surface? = null
    private var drainThread: Thread? = null
    private var feedThread: Thread? = null
    @Volatile private var running = false

    /** Source dimensions, for the agent's stream setup. */
    var srcWidth = 0; private set
    var srcHeight = 0; private set

    fun probe(): Boolean {
        val ex = MediaExtractor()
        try { ex.setDataSource(ctx, uri, null) } catch (e: Exception) { Log.w(TAG, "open", e); ex.release(); return false }
        for (i in 0 until ex.trackCount) {
            val f = ex.getTrackFormat(i)
            if ((f.getString(MediaFormat.KEY_MIME) ?: "").startsWith("video/")) {
                srcWidth = f.getInteger(MediaFormat.KEY_WIDTH)
                srcHeight = f.getInteger(MediaFormat.KEY_HEIGHT)
                ex.release(); return true
            }
        }
        ex.release(); return false
    }

    fun start() {
        if (running) return
        val ex = MediaExtractor()
        ex.setDataSource(ctx, uri, null)
        var track = -1; var inFormat: MediaFormat? = null
        for (i in 0 until ex.trackCount) {
            val f = ex.getTrackFormat(i)
            if ((f.getString(MediaFormat.KEY_MIME) ?: "").startsWith("video/")) { track = i; inFormat = f; break }
        }
        if (track < 0 || inFormat == null) { Log.w(TAG, "no video track"); ex.release(); return }
        ex.selectTrack(track); extractor = ex
        srcWidth = inFormat.getInteger(MediaFormat.KEY_WIDTH)
        srcHeight = inFormat.getInteger(MediaFormat.KEY_HEIGHT)
        if (width <= 0) width = srcWidth
        if (height <= 0) height = srcHeight

        val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        enc.configure(encFormat(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        surface = enc.createInputSurface()
        enc.start(); encoder = enc

        val dec = MediaCodec.createDecoderByType(inFormat.getString(MediaFormat.KEY_MIME)!!)
        dec.configure(inFormat, surface, null, 0)   // render decoded frames onto the encoder surface
        dec.start(); decoder = dec

        running = true
        drainThread = Thread({ drainLoop() }, "bsdr-file-enc").also { it.start() }
        feedThread = Thread({ feedLoop() }, "bsdr-file-dec").also { it.start() }
        Log.i(TAG, "file source ${srcWidth}x$srcHeight -> ${width}x$height @ ${bitrate}bps")
    }

    private fun encFormat(): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, if (bitrate > 0) bitrate else 8_000_000)
            setInteger(MediaFormat.KEY_FRAME_RATE, if (fps > 0) fps else 30)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        }

    /** decoder: extractor samples in, decoded frames rendered to the encoder surface, paced to PTS. */
    private fun feedLoop() {
        val ex = extractor ?: return
        val dec = decoder ?: return
        val info = MediaCodec.BufferInfo()
        var startWallNs = 0L; var startPtsUs = -1L
        var sawInputEos = false
        while (running) {
            if (!sawInputEos) {
                val inIdx = try { dec.dequeueInputBuffer(10_000) } catch (e: Exception) { -1 }
                if (inIdx >= 0) {
                    val buf = dec.getInputBuffer(inIdx)!!
                    val n = ex.readSampleData(buf, 0)
                    if (n < 0) {                                   // EOS -> loop the file
                        ex.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC)
                        startPtsUs = -1
                        dec.queueInputBuffer(inIdx, 0, 0, 0, 0)    // (harmless empty push; keep going)
                    } else {
                        dec.queueInputBuffer(inIdx, 0, n, ex.sampleTime, 0)
                        ex.advance()
                    }
                }
            }
            val outIdx = try { dec.dequeueOutputBuffer(info, 10_000) } catch (e: Exception) { -1 }
            if (outIdx >= 0) {
                // pace: hold back rendering to roughly match the file's timeline
                if (info.presentationTimeUs > 0) {
                    if (startPtsUs < 0) { startPtsUs = info.presentationTimeUs; startWallNs = System.nanoTime() }
                    val targetNs = startWallNs + (info.presentationTimeUs - startPtsUs) * 1000
                    val sleepMs = (targetNs - System.nanoTime()) / 1_000_000
                    if (sleepMs in 1..500) try { Thread.sleep(sleepMs) } catch (_: InterruptedException) {}
                }
                dec.releaseOutputBuffer(outIdx, info.size > 0)     // render to the encoder surface
            }
        }
        try { encoder?.signalEndOfInputStream() } catch (_: Exception) {}
    }

    /** encoder: Annex-B access units -> native, honouring live quality changes. */
    private fun drainLoop() {
        val info = MediaCodec.BufferInfo()
        val want = IntArray(4)
        while (running) {
            val c = encoder ?: break
            if (NativeBridge.nativePollVideoWant(want) && want[3] > 0 && want[3] != bitrate) {
                bitrate = want[3]
                try { c.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate) }) } catch (_: Exception) {}
            }
            val idx = try { c.dequeueOutputBuffer(info, 10_000) } catch (e: Exception) { -1 }
            if (idx < 0) continue
            val buf = c.getOutputBuffer(idx)
            if (buf != null && info.size > 0) {
                buf.position(info.offset); buf.limit(info.offset + info.size)
                val bytes = ByteArray(info.size); buf.get(bytes)
                val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                NativeBridge.nativePushVideo(bytes, bytes.size, info.presentationTimeUs, isConfig)
            }
            c.releaseOutputBuffer(idx, false)
        }
    }

    fun stop() {
        running = false
        try { feedThread?.join(500) } catch (_: InterruptedException) {}
        try { drainThread?.join(500) } catch (_: InterruptedException) {}
        try { decoder?.stop(); decoder?.release() } catch (_: Exception) {}
        try { encoder?.stop(); encoder?.release() } catch (_: Exception) {}
        try { surface?.release() } catch (_: Exception) {}
        try { extractor?.release() } catch (_: Exception) {}
        decoder = null; encoder = null; surface = null; extractor = null
    }

    companion object { private const val TAG = "bsdr.filesrc" }
}
