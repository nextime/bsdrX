/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import androidx.core.content.ContextCompat

/**
 * Camera2 -> (optional 2D->3D SBS GL stage) -> MediaCodec AVC encoder -> native, mirroring
 * ScreenCapture but sourced from a camera instead of the screen. The camera renders into the same
 * encoder input surface (or the SBS pipeline's SurfaceTexture) that the projection path uses, so the
 * whole downstream (encoder, quality/3D polling, native push) is shared. Single camera only; the
 * phone can't run a true two-camera stereo pair on the same scene, so webcam3d uses depth-based 3D.
 */
class CameraCapture(
    private val ctx: Context,
    private val cameraId: String,
    private var fps: Int,
    private var bitrate: Int,
) {
    private var codec: MediaCodec? = null
    private var surface: Surface? = null          // codec input surface
    private var camTarget: Surface? = null        // where the camera renders (SBS input or codec input)
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var thread: Thread? = null
    private var camThread: HandlerThread? = null
    private var camHandler: Handler? = null
    @Volatile private var running = false
    private var width = 1280
    private var height = 720

    private var sbs: SbsGlPipeline? = null
    private var tdMode = 0
    private var tdDeep = 35
    private var tdConv = 0
    private var tdSwap = 0
    private var tdFull = 1
    private var tdTier = 0
    private val td = IntArray(6)
    @Volatile private var fsOn = false

    fun setFaceswap(on: Boolean) {
        if (fsOn == on) return
        val needRecreate = (tdMode == 0)
        fsOn = on
        sbs?.setFaceswap(on)
        if (needRecreate) recreate()
    }

    fun start() {
        if (running) return
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "CAMERA permission not granted; cannot start webcam")
            return
        }
        running = true
        pickSize()
        camThread = HandlerThread("bsdr-cam").also { it.start() }
        camHandler = Handler(camThread!!.looper)
        openCodec()
        openCamera()
        thread = Thread({ drainLoop() }, "bsdr-encode-cam").also { it.start() }
        Log.i(TAG, "webcam capture start: cam=$cameraId ${width}x$height @${fps}")
    }

    /** Pick a camera output size the encoder can take: prefer 1280x720, else the largest <=1080p. */
    private fun pickSize() {
        try {
            val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val map = mgr.getCameraCharacteristics(cameraId)
                .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val sizes = map?.getOutputSizes(MediaCodec::class.java)
                ?: map?.getOutputSizes(ImageFormat.YUV_420_888)
            var best: Size? = null
            sizes?.forEach { s ->
                if (s.width == 1280 && s.height == 720) best = s
                else if ((best == null || best!!.width * best!!.height < s.width * s.height) &&
                    s.width <= 1920 && s.height <= 1080) best = s
            }
            best?.let { width = it.width and 1.inv(); height = it.height and 1.inv() }
        } catch (e: Exception) { Log.w(TAG, "pickSize: ${e.message}") }
    }

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
        // 2D->3D: splice a GL SBS stage between the camera and the encoder. On any GL failure, flat.
        camTarget = if (tdMode != 0 || fsOn) {
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
    }

    private fun openCamera() {
        val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        try {
            mgr.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(dev: CameraDevice) { camera = dev; startSession(dev) }
                override fun onDisconnected(dev: CameraDevice) { dev.close(); camera = null }
                override fun onError(dev: CameraDevice, err: Int) {
                    Log.e(TAG, "camera $cameraId error $err"); dev.close(); camera = null
                }
            }, camHandler)
        } catch (e: SecurityException) {
            Log.e(TAG, "CAMERA permission missing: ${e.message}")
        } catch (e: Exception) {
            Log.e(TAG, "openCamera($cameraId): ${e.message}")
        }
    }

    private fun startSession(dev: CameraDevice) {
        val tgt = camTarget ?: return
        try {
            val req = dev.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(tgt)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(fps, fps))
            }
            @Suppress("DEPRECATION")
            dev.createCaptureSession(listOf(tgt), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    session = s
                    try { s.setRepeatingRequest(req.build(), null, camHandler) }
                    catch (e: Exception) { Log.e(TAG, "repeatingRequest: ${e.message}") }
                }
                override fun onConfigureFailed(s: CameraCaptureSession) { Log.e(TAG, "session config failed") }
            }, camHandler)
        } catch (e: Exception) { Log.e(TAG, "startSession: ${e.message}") }
    }

    private fun closeCamera() {
        try { session?.close() } catch (_: Exception) {}
        try { camera?.close() } catch (_: Exception) {}
        session = null; camera = null
    }

    private fun closeCodec() {
        try { sbs?.release() } catch (_: Exception) {}
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        try { surface?.release() } catch (_: Exception) {}
        sbs = null; codec = null; surface = null; camTarget = null
    }

    /** Live bitrate via setParameters; a 3D on/off or resolution change recreates the whole chain.
     *  w/h are unused here (they mirror the native want[] 4-int contract; resolution changes come in
     *  via the 3D path -> recreate()), but kept so the signature matches nativePollVideoWant's layout. */
    @Suppress("UNUSED_PARAMETER")
    private fun applyWant(w: Int, h: Int, f: Int, br: Int) {
        if (br > 0 && br != bitrate) {
            bitrate = br
            codec?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate)
            })
        }
        if (f > 0) fps = f
    }

    private fun recreate() {
        closeCamera(); closeCodec(); openCodec(); openCamera()
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
                if ((tdMode != 0) != wasOn) { Log.i(TAG, "2D->3D ${if (tdMode != 0) "on" else "off"} (recreating)"); recreate() }
                else sbs?.setParams(tdMode, tdDeep, tdConv, tdSwap, tdTier)
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
        try { thread?.join(500) } catch (_: InterruptedException) {}
        closeCamera(); closeCodec()
        try { camThread?.quitSafely() } catch (_: Exception) {}
        camThread = null; camHandler = null
    }

    companion object { private const val TAG = "bsdr.capture" }
}
