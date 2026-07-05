/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Bundle
import android.util.Log
import android.view.Surface

/**
 * MediaProjection -> a MediaCodec AVC encoder (Constrained Baseline, CBR, realtime,
 * to match the Quest's OpenH264 receiver) -> Annex-B access units pushed to native
 * via NativeBridge.nativePushVideo. Honours live quality changes the headset makes
 * (PUT /device), polled from native through nativePollVideoWant.
 */
class ScreenCapture(
    private val projection: MediaProjection,
    private var width: Int,
    private var height: Int,
    private val dpi: Int,
    private var fps: Int,
    private var bitrate: Int,
) {
    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    private var display: VirtualDisplay? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    /** 2D->3D: when mode != 0, MediaProjection renders through an SBS GL stage into the encoder. */
    private var sbs: SbsGlPipeline? = null
    private var tdMode = 0
    private var tdDeep = 35
    private var tdConv = 0
    private var tdSwap = 0
    private var tdFull = 1
    private var tdTier = 0
    private val td = IntArray(6)
    @Volatile private var fsOn = false   // face swap active (routes through the GL stage)

    /** Enable/disable the GL face-swap stage. Recreates the codec if that changes whether a GL stage
     *  is needed at all (it's shared with SBS). */
    fun setFaceswap(on: Boolean) {
        if (fsOn == on) return
        val needRecreate = (tdMode == 0)   // without 3D, turning fs on/off adds/removes the GL stage
        fsOn = on
        sbs?.setFaceswap(on)
        if (needRecreate) { closeCodec(); openCodec() }
    }

    fun start() {
        if (running) return
        running = true
        openCodec()
        thread = Thread({ drainLoop() }, "bsdr-encode").also { it.start() }
    }

    /** Encoded frame width. Always the display width: half-SBS keeps the screen's aspect (a 2x-wide
     * frame makes the Quest's screen double-wide, not 3D), and on a phone the projection is already at
     * display res so a higher-res "full" encode would only upscale. So `full` is a no-op here. */
    private fun encWidth(): Int = width

    private fun makeFormat(encW: Int): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, encW, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline)
            setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_LATENCY, 1)
        }

    private fun openCodec() {
        val encW = encWidth()
        val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        c.configure(makeFormat(encW), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val codecSurface = c.createInputSurface()
        surface = codecSurface
        c.start()
        codec = c
        /* 2D->3D: when enabled, splice a GL SBS stage between the projection (rendered at the display
         * size) and the encoder input (encW wide). On any GL failure, fall back to a flat encode. */
        val target: Surface = if (tdMode != 0 || fsOn) {
            try {
                SbsGlPipeline(codecSurface, width, encW, height).also {
                    it.setParams(tdMode, tdDeep, tdConv, tdSwap, tdTier)
                    it.setFaceswap(fsOn)
                    sbs = it
                }.inputSurface
            } catch (e: Exception) {
                Log.w(TAG, "GL pipeline unavailable -> flat: ${e.message}")
                sbs = null
                codecSurface
            }
        } else codecSurface
        // Android 14+ forbids calling createVirtualDisplay more than once on the same MediaProjection.
        // Create it ONCE and, on every later codec recreate (resolution / 3D toggle), just resize it
        // and re-point it at the new encoder surface.
        val d = display
        if (d == null) {
            display = projection.createVirtualDisplay(
                "bsdr-screen", width, height, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, target, null, null)
        } else {
            d.resize(width, height, dpi)
            d.surface = target
        }
    }

    private fun closeCodec() {
        try { display?.surface = null } catch (_: Exception) {}   // detach before releasing the surface (keep the display)
        try { sbs?.release() } catch (_: Exception) {}   // stop GL before releasing the codec surface
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        try { surface?.release() } catch (_: Exception) {}
        sbs = null; codec = null; surface = null
    }

    /** Live bitrate via setParameters; resolution change requires a codec recreate. */
    private fun applyWant(w: Int, h: Int, f: Int, br: Int) {
        if (br > 0 && br != bitrate) {
            bitrate = br
            codec?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate)
            })
        }
        if (f > 0) fps = f
        if ((w > 0 && w != width) || (h > 0 && h != height)) {
            width = if (w > 0) w else width
            height = if (h > 0) h else height
            Log.i(TAG, "resolution -> ${width}x$height (recreating codec)")
            closeCodec(); openCodec()
        }
    }

    private fun drainLoop() {
        val info = MediaCodec.BufferInfo()
        val want = IntArray(4)
        while (running) {
            val c = codec ?: break
            if (NativeBridge.nativePollVideoWant(want)) applyWant(want[0], want[1], want[2], want[3])
            if (NativeBridge.nativePollThreed(td)) {
                val wasOn = tdMode != 0
                tdMode = td[0]; tdDeep = td[1]; tdConv = td[2]; tdSwap = td[3]; tdFull = td[4]; tdTier = td[5]
                if ((tdMode != 0) != wasOn) {   // on/off recreates the pipeline; params retune live
                    Log.i(TAG, "2D->3D ${if (tdMode != 0) "on" else "off"} (recreating)")
                    closeCodec(); openCodec()
                } else {
                    sbs?.setParams(tdMode, tdDeep, tdConv, tdSwap, tdTier)
                }
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

    /** Pause encoding but KEEP the VirtualDisplay (its createVirtualDisplay can't be re-issued on the
     *  same MediaProjection). Used when switching the source to a webcam; resume() with start(). */
    fun pause() {
        if (!running) return
        running = false
        try { thread?.join(500) } catch (_: InterruptedException) {}
        closeCodec()   // keeps `display`; its surface is detached
    }

    fun stop() {
        running = false
        try { thread?.join(500) } catch (_: InterruptedException) {}
        closeCodec()
        try { display?.release() } catch (_: Exception) {}   // release the VirtualDisplay only on full stop
        display = null
    }

    companion object { private const val TAG = "bsdr.capture" }
}
