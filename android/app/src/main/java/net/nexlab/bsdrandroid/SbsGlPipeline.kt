/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Inserts a GLES stage between MediaProjection and the MediaCodec encoder input surface to convert
 * the screen to real-time side-by-side (SBS) 3D — the Android counterpart of the desktop threed.c.
 *
 * Wiring:  MediaProjection --renders into--> [inputSurface]  (external-OES SurfaceTexture)
 *          on each frame, a GL thread runs the SBS shader and draws to [outputSurface]
 *          (the encoder input surface), then presents with the source timestamp.
 *
 * The SBS math mirrors threed.c: a light per-pixel heuristic depth (vertical gradient + luma) drives
 * a horizontal parallax shift, each eye squished into one half of the frame. All work is on the GPU,
 * so it's cheap even on phones. When mode == 0 the shader is a straight passthrough.
 *
 * Construction sets up EGL/GL on a dedicated thread and throws on failure so the caller can fall
 * back to rendering MediaProjection straight to the encoder.
 */
class SbsGlPipeline(
    private val outputSurface: Surface,
    private val srcWidth: Int,   // MediaProjection renders here (display width)
    private val outWidth: Int,   // encoder frame width (2x srcWidth for full-res per eye)
    private val height: Int,
) {
    private val glThread = HandlerThread("bsdr-sbs-gl").apply { start() }
    private val glHandler = Handler(glThread.looper)

    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

    private var program = 0
    private var texId = 0
    private var aPos = 0
    private var aTex = 0
    private var uTexMatrix = 0
    private var uMode = 0
    private var uAmp = 0
    private var uC0 = 0
    private var uSwap = 0
    private var uUseDepth = 0
    private var uDepthTex = 0

    // ---- in-process neural depth (tier > 0): a small frame is read back on the GL thread, inferred
    // on a worker thread (NNAPI via the C engine), and the resulting grid uploaded as a texture the
    // SBS shader samples instead of the gradient+luma heuristic. dw/dh = the readback/grid size.
    private val dw = 256
    private var dh = 144
    private var depthTex = 0            // GL_LUMINANCE dw*dh depth grid (0..255, near=255)
    private var readFbo = 0            // FBO the OES frame is downscaled into for readback
    private var readColorTex = 0
    private lateinit var rgba: ByteBuffer          // dw*dh*4 readback target
    private lateinit var grayArr: ByteArray        // dw*dh luma, right-side-up, fed to C
    private lateinit var gridArr: FloatArray       // dw*dh depth from C (0..1, near=1)
    private lateinit var lumBuf: ByteBuffer        // dw*dh bytes uploaded to depthTex
    private var frameCount = 0
    private val depthBusy = AtomicBoolean(false)   // an inference job is in flight
    @Volatile private var gridPending = false      // worker produced a new grid; upload it on GL thread
    @Volatile private var depthReady = false       // depthTex holds a valid grid → shader may sample it
    private val depthThread = HandlerThread("bsdr-depth").apply { start() }
    private val depthHandler = Handler(depthThread.looper)

    private lateinit var surfaceTexture: SurfaceTexture
    /** MediaProjection's VirtualDisplay renders here; valid after construction. */
    lateinit var inputSurface: Surface
        private set

    // live SBS params (read on the GL thread each frame)
    @Volatile private var pMode = 1
    @Volatile private var pAmp = 0.35f * 0.04f
    @Volatile private var pC0 = 0.5f
    @Volatile private var pSwap = 0f
    @Volatile private var pTier = 0

    // ---- face swap (full-frame readback -> C ONNX swap -> re-upload -> draw). Synchronous on the GL
    // thread when on, so the encoded framerate drops to the swap rate but every frame is coherent. ----
    @Volatile private var pFaceswap = false
    private val fsW = (if (srcWidth < 960) srcWidth else 960) and 1.inv()
    private var fsH = 0
    private var fsFbo = 0
    private var fsColorTex = 0                      // OES rendered here for readback (RGBA)
    private var fsTex = 0                           // swapped RGB uploaded here, drawn to the encoder
    private lateinit var fsRgba: ByteBuffer         // fsW*fsH*4 readback
    private lateinit var fsRgb: ByteArray           // fsW*fsH*3 top-down, swapped in place by C
    private lateinit var fsUp: ByteBuffer           // fsW*fsH*3 upload buffer
    private var prog2d = 0                          // samples a 2D RGB texture (the swapped frame)
    private var a2Pos = 0; private var a2Tex = 0; private var u2Tex = 0

    private val texMatrix = FloatArray(16)
    private val quad: FloatBuffer = ByteBuffer
        .allocateDirect(QUAD.size * 4).order(ByteOrder.nativeOrder())
        .asFloatBuffer().apply { put(QUAD); position(0) }

    init {
        val latch = CountDownLatch(1)
        var err: Throwable? = null
        glHandler.post {
            try {
                initGl()
            } catch (t: Throwable) {
                err = t
            } finally {
                latch.countDown()
            }
        }
        latch.await()
        err?.let { release(); throw RuntimeException("SBS GL init failed", it) }
    }

    /** Update the SBS parameters. tier>0 turns on in-process neural depth (else the GL heuristic). */
    fun setParams(mode: Int, deepness: Int, convergence: Int, swap: Int, tier: Int = 0) {
        pMode = if (mode < 0) 0 else mode
        pAmp = (deepness.coerceIn(0, 100) / 100f) * 0.04f
        pC0 = 0.5f - convergence.coerceIn(-50, 50) / 100f
        pSwap = if (swap != 0) 1f else 0f
        pTier = tier.coerceIn(0, 3)
    }

    /** Turn the GL face-swap stage on/off (the engine + source are managed natively). */
    fun setFaceswap(on: Boolean) { pFaceswap = on }

    // ---- GL thread ----------------------------------------------------------------------------
    private fun initGl() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        require(eglDisplay != EGL14.EGL_NO_DISPLAY) { "eglGetDisplay" }
        val ver = IntArray(2)
        require(EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1)) { "eglInitialize" }

        val cfgAttr = intArrayOf(
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL_RECORDABLE_ANDROID, 1,
            EGL14.EGL_NONE,
        )
        val cfgs = arrayOfNulls<EGLConfig>(1)
        val nCfg = IntArray(1)
        require(EGL14.eglChooseConfig(eglDisplay, cfgAttr, 0, cfgs, 0, 1, nCfg, 0) && nCfg[0] > 0) {
            "eglChooseConfig"
        }
        eglContext = EGL14.eglCreateContext(
            eglDisplay, cfgs[0], EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
        )
        require(eglContext != EGL14.EGL_NO_CONTEXT) { "eglCreateContext" }
        eglSurface = EGL14.eglCreateWindowSurface(
            eglDisplay, cfgs[0], outputSurface, intArrayOf(EGL14.EGL_NONE), 0,
        )
        require(eglSurface != EGL14.EGL_NO_SURFACE) { "eglCreateWindowSurface" }
        require(EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) { "eglMakeCurrent" }
        GLES20.glClearColor(0f, 0f, 0f, 1f)   // letterbox/pillarbox bars are opaque black

        program = buildProgram()
        aPos = GLES20.glGetAttribLocation(program, "aPos")
        aTex = GLES20.glGetAttribLocation(program, "aTex")
        uTexMatrix = GLES20.glGetUniformLocation(program, "uTexMatrix")
        uMode = GLES20.glGetUniformLocation(program, "uMode")
        uAmp = GLES20.glGetUniformLocation(program, "uAmp")
        uC0 = GLES20.glGetUniformLocation(program, "uC0")
        uSwap = GLES20.glGetUniformLocation(program, "uSwap")
        uUseDepth = GLES20.glGetUniformLocation(program, "uUseDepth")
        uDepthTex = GLES20.glGetUniformLocation(program, "uDepthTex")

        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        texId = tex[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        // ---- neural-depth resources: FBO for the downscaled readback + the depth grid texture ----
        dh = (dw.toLong() * height / srcWidth).toInt().coerceIn(64, 256)
        dh = dh and 1.inv()                                  // keep even
        rgba = ByteBuffer.allocateDirect(dw * dh * 4).order(ByteOrder.nativeOrder())
        grayArr = ByteArray(dw * dh)
        gridArr = FloatArray(dw * dh)
        lumBuf = ByteBuffer.allocateDirect(dw * dh).order(ByteOrder.nativeOrder())

        val t2 = IntArray(2)
        GLES20.glGenTextures(2, t2, 0)
        depthTex = t2[0]; readColorTex = t2[1]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, depthTex)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE, dw, dh, 0,
            GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, null)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, readColorTex)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, dw, dh, 0,
            GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        val fb = IntArray(1); GLES20.glGenFramebuffers(1, fb, 0); readFbo = fb[0]
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, readFbo)
        GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0,
            GLES20.GL_TEXTURE_2D, readColorTex, 0)
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)

        // ---- face-swap resources: a full working-res FBO for readback + the swapped-frame texture ----
        fsH = (height.toLong() * fsW / srcWidth).toInt() and 1.inv()
        fsRgba = ByteBuffer.allocateDirect(fsW * fsH * 4).order(ByteOrder.nativeOrder())
        fsRgb = ByteArray(fsW * fsH * 3)
        fsUp = ByteBuffer.allocateDirect(fsW * fsH * 3).order(ByteOrder.nativeOrder())
        val ft = IntArray(2); GLES20.glGenTextures(2, ft, 0)
        fsColorTex = ft[0]; fsTex = ft[1]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fsColorTex)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, fsW, fsH, 0,
            GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fsTex)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGB, fsW, fsH, 0,
            GLES20.GL_RGB, GLES20.GL_UNSIGNED_BYTE, null)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        val ffb = IntArray(1); GLES20.glGenFramebuffers(1, ffb, 0); fsFbo = ffb[0]
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fsFbo)
        GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0,
            GLES20.GL_TEXTURE_2D, fsColorTex, 0)
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
        prog2d = buildProgram2d()
        a2Pos = GLES20.glGetAttribLocation(prog2d, "aPos")
        a2Tex = GLES20.glGetAttribLocation(prog2d, "aTex")
        u2Tex = GLES20.glGetUniformLocation(prog2d, "s2")

        surfaceTexture = SurfaceTexture(texId).apply {
            setDefaultBufferSize(srcWidth, height)
            setOnFrameAvailableListener({ drawFrame() }, glHandler)
        }
        inputSurface = Surface(surfaceTexture)
    }

    /** Face-swap path: render the OES frame into fsFbo, read it back, swap faces (blocking C call),
     *  upload the result and draw it to the encoder. Returns false if it couldn't run (caller draws
     *  the normal path). Runs entirely on the GL thread. */
    private fun drawFaceswap(): Boolean {
        try {
            // 1) OES -> fsFbo passthrough (downscaled to the working size)
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fsFbo)
            GLES20.glViewport(0, 0, fsW, fsH)
            GLES20.glUseProgram(program)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
            quad.position(0); GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad)
            quad.position(2); GLES20.glEnableVertexAttribArray(aTex)
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quad)
            GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)
            GLES20.glUniform1i(uMode, 0); GLES20.glUniform1i(uUseDepth, 0)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            GLES20.glDisableVertexAttribArray(aPos); GLES20.glDisableVertexAttribArray(aTex)

            // 2) readback + flip bottom-up -> top-down RGB (faces must be upright for the detector)
            fsRgba.clear()
            GLES20.glReadPixels(0, 0, fsW, fsH, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, fsRgba)
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            for (row in 0 until fsH) {
                val s = (fsH - 1 - row) * fsW * 4
                val d = row * fsW * 3
                for (col in 0 until fsW) {
                    val p = s + col * 4
                    fsRgb[d + col*3]     = fsRgba.get(p)
                    fsRgb[d + col*3 + 1] = fsRgba.get(p + 1)
                    fsRgb[d + col*3 + 2] = fsRgba.get(p + 2)
                }
            }
            // 3) swap in place (blocks the GL thread; that just lowers the framerate)
            NativeBridge.nativeFaceswapProcess(fsRgb, fsW, fsH)
            // 4) upload + draw (flip V back in the shader)
            fsUp.clear(); fsUp.put(fsRgb); fsUp.position(0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fsTex)
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, fsW, fsH,
                GLES20.GL_RGB, GLES20.GL_UNSIGNED_BYTE, fsUp)
            GLES20.glViewport(0, 0, outWidth, height)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glUseProgram(prog2d)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fsTex)
            GLES20.glUniform1i(u2Tex, 0)
            quad.position(0); GLES20.glEnableVertexAttribArray(a2Pos)
            GLES20.glVertexAttribPointer(a2Pos, 2, GLES20.GL_FLOAT, false, 16, quad)
            quad.position(2); GLES20.glEnableVertexAttribArray(a2Tex)
            GLES20.glVertexAttribPointer(a2Tex, 2, GLES20.GL_FLOAT, false, 16, quad)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            GLES20.glDisableVertexAttribArray(a2Pos); GLES20.glDisableVertexAttribArray(a2Tex)
            return true
        } catch (e: Exception) {
            Log.w(TAG, "faceswap draw failed: ${e.message}")
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            return false
        }
    }

    private fun drawFrame() {
        try {
            surfaceTexture.updateTexImage()
            surfaceTexture.getTransformMatrix(texMatrix)

            // Face swap replaces the whole frame -> its own render path (present + return).
            if (pFaceswap && drawFaceswap()) {
                EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, surfaceTexture.timestamp)
                EGL14.eglSwapBuffers(eglDisplay, eglSurface)
                return
            }

            // A freshly-inferred depth grid waiting? Upload it to depthTex (GL thread only).
            if (gridPending) {
                lumBuf.clear()
                for (i in 0 until dw * dh) {
                    val v = (gridArr[i] * 255f).toInt().coerceIn(0, 255)
                    lumBuf.put(i, v.toByte())
                }
                lumBuf.position(0)
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, depthTex)
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, dw, dh,
                    GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, lumBuf)
                gridPending = false
                depthReady = true
            }

            val useDepth = if (pTier > 0 && depthReady) 1 else 0

            GLES20.glViewport(0, 0, outWidth, height)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glUseProgram(program)

            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, depthTex)

            quad.position(0)
            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad)
            quad.position(2)
            GLES20.glEnableVertexAttribArray(aTex)
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quad)

            GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)
            GLES20.glUniform1i(uMode, pMode)
            GLES20.glUniform1f(uAmp, pAmp)
            GLES20.glUniform1f(uC0, pC0)
            GLES20.glUniform1f(uSwap, pSwap)
            GLES20.glUniform1i(uUseDepth, useDepth)
            GLES20.glUniform1i(uDepthTex, 1)

            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

            GLES20.glDisableVertexAttribArray(aPos)
            GLES20.glDisableVertexAttribArray(aTex)

            EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, surfaceTexture.timestamp)
            EGL14.eglSwapBuffers(eglDisplay, eglSurface)

            // Neural-depth pump: every Nth frame, if no job is in flight, read back a small frame and
            // infer on the worker thread. Keeps the heavy model off the GL/render path.
            frameCount++
            if (pMode != 0 && pTier > 0 && frameCount % DEPTH_EVERY == 0 &&
                depthBusy.compareAndSet(false, true)) {
                if (readbackGray()) {
                    val tier = pTier
                    depthHandler.post {
                        val ok = try {
                            NativeBridge.nativeDepth(tier, grayArr, dw, dh, gridArr)
                        } catch (t: Throwable) { false }
                        if (ok) gridPending = true
                        depthBusy.set(false)
                    }
                } else {
                    depthBusy.set(false)
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "SBS draw failed: ${e.message}")
        }
    }

    /** Render the current OES frame passthrough into the small readback FBO, then pull it back and
     *  fill grayArr (right-side-up luma) for the depth model. Runs on the GL thread. */
    private fun readbackGray(): Boolean {
        try {
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, readFbo)
            GLES20.glViewport(0, 0, dw, dh)
            GLES20.glUseProgram(program)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
            quad.position(0)
            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad)
            quad.position(2)
            GLES20.glEnableVertexAttribArray(aTex)
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quad)
            GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)
            GLES20.glUniform1i(uMode, 0)        // passthrough (plain downscaled copy)
            GLES20.glUniform1i(uUseDepth, 0)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            GLES20.glDisableVertexAttribArray(aPos)
            GLES20.glDisableVertexAttribArray(aTex)

            rgba.clear()
            GLES20.glReadPixels(0, 0, dw, dh, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba)
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)   // restore window framebuffer

            // glReadPixels rows are bottom-up; flip to top-down so the model sees an upright frame.
            for (row in 0 until dh) {
                val srcBase = (dh - 1 - row) * dw * 4
                val dstBase = row * dw
                for (col in 0 until dw) {
                    val p = srcBase + col * 4
                    val r = rgba.get(p).toInt() and 0xff
                    val g = rgba.get(p + 1).toInt() and 0xff
                    val b = rgba.get(p + 2).toInt() and 0xff
                    grayArr[dstBase + col] = ((r * 77 + g * 150 + b * 29) shr 8).toByte()
                }
            }
            return true
        } catch (e: Exception) {
            Log.w(TAG, "depth readback failed: ${e.message}")
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            return false
        }
    }

    fun release() {
        try {
            depthThread.quitSafely()
        } catch (_: Exception) {}
        try {
            val done = CountDownLatch(1)
            glHandler.post {
                try {
                    if (::surfaceTexture.isInitialized) surfaceTexture.release()
                    if (readFbo != 0) GLES20.glDeleteFramebuffers(1, intArrayOf(readFbo), 0)
                    if (fsFbo != 0) GLES20.glDeleteFramebuffers(1, intArrayOf(fsFbo), 0)
                    if (depthTex != 0 || readColorTex != 0)
                        GLES20.glDeleteTextures(2, intArrayOf(depthTex, readColorTex), 0)
                    if (fsColorTex != 0 || fsTex != 0)
                        GLES20.glDeleteTextures(2, intArrayOf(fsColorTex, fsTex), 0)
                    if (prog2d != 0) GLES20.glDeleteProgram(prog2d)
                    if (program != 0) GLES20.glDeleteProgram(program)
                    if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                        EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                        if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, eglSurface)
                        if (eglContext != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDisplay, eglContext)
                        EGL14.eglTerminate(eglDisplay)
                    }
                } catch (_: Exception) {} finally { done.countDown() }
            }
            done.await()
            if (::inputSurface.isInitialized) inputSurface.release()
        } catch (_: Exception) {}
        glThread.quitSafely()
    }

    private fun buildProgram(): Int {
        val vs = compile(GLES20.GL_VERTEX_SHADER, VERT)
        val fs = compile(GLES20.GL_FRAGMENT_SHADER, FRAG)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        require(ok[0] != 0) { "link: " + GLES20.glGetProgramInfoLog(p) }
        GLES20.glDeleteShader(vs); GLES20.glDeleteShader(fs)
        return p
    }

    /** A minimal program that draws a 2D RGB texture (the swapped frame), flipping V so the top-down
     *  readback data comes out upright. */
    private fun buildProgram2d(): Int {
        val vs = compile(GLES20.GL_VERTEX_SHADER,
            "attribute vec4 aPos;\nattribute vec2 aTex;\nvarying vec2 vTex;\n" +
            "void main(){ gl_Position = aPos; vTex = vec2(aTex.x, 1.0 - aTex.y); }\n")
        val fs = compile(GLES20.GL_FRAGMENT_SHADER,
            "precision mediump float;\nvarying vec2 vTex;\nuniform sampler2D s2;\n" +
            "void main(){ gl_FragColor = texture2D(s2, vTex); }\n")
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs); GLES20.glAttachShader(p, fs); GLES20.glLinkProgram(p)
        val ok = IntArray(1); GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        require(ok[0] != 0) { "link2d: " + GLES20.glGetProgramInfoLog(p) }
        GLES20.glDeleteShader(vs); GLES20.glDeleteShader(fs)
        return p
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        require(ok[0] != 0) { "compile: " + GLES20.glGetShaderInfoLog(s) }
        return s
    }

    companion object {
        private const val TAG = "bsdr.sbs"
        private const val EGL_RECORDABLE_ANDROID = 0x3142
        private const val DEPTH_EVERY = 6         // infer neural depth every Nth frame (~5 Hz @ 30 fps)

        // triangle strip: x, y, u, v  (u,v = 0..1 across the OUTPUT/packed frame)
        private val QUAD = floatArrayOf(
            -1f, -1f, 0f, 0f,
            1f, -1f, 1f, 0f,
            -1f, 1f, 0f, 1f,
            1f, 1f, 1f, 1f,
        )

        private const val VERT =
            "attribute vec4 aPos;\n" +
            "attribute vec2 aTex;\n" +
            "varying vec2 vTex;\n" +
            "void main(){ gl_Position = aPos; vTex = aTex; }\n"

        /* SBS DIBR, mirroring threed.c: eye = which half; sample the source squished 2:1 with a
         * per-pixel horizontal shift from a heuristic depth (vertical gradient + luma). uTexMatrix
         * maps our normalized uv to the external texture's real coords. mode 0 = passthrough. */
        private const val FRAG =
            "#extension GL_OES_EGL_image_external : require\n" +
            "precision mediump float;\n" +
            "varying vec2 vTex;\n" +
            "uniform samplerExternalOES sTex;\n" +
            "uniform mat4 uTexMatrix;\n" +
            "uniform int uMode;\n" +
            "uniform float uAmp;\n" +
            "uniform float uC0;\n" +
            "uniform float uSwap;\n" +
            "uniform int uUseDepth;\n" +
            "uniform sampler2D uDepthTex;\n" +
            "vec2 tx(vec2 uv){ return (uTexMatrix * vec4(uv, 0.0, 1.0)).xy; }\n" +
            "void main(){\n" +
            "  vec2 uv = vTex;\n" +
            "  if(uMode == 0){ gl_FragColor = texture2D(sTex, tx(uv)); return; }\n" +
            "  bool left = uv.x < 0.5;\n" +
            "  float eu = left ? uv.x * 2.0 : (uv.x - 0.5) * 2.0;\n" +
            "  float lum = dot(texture2D(sTex, tx(vec2(eu, uv.y))).rgb, vec3(0.299,0.587,0.114));\n" +
            // Neural depth grid (top-down, 0..1 near=1) sampled at the frame position, else the
            // heuristic: vTex.y = 0 is the bottom of the output -> near; top -> far (matches threed.c).
            "  float d;\n" +
            "  if(uUseDepth == 1){ d = texture2D(uDepthTex, vec2(eu, 1.0 - uv.y)).r; }\n" +
            "  else { d = clamp(0.6 * (1.0 - uv.y) + 0.4 * lum, 0.0, 1.0); }\n" +
            "  float mag = uAmp * (d - uC0) * 0.5;\n" +
            "  float dir = left ? 1.0 : -1.0;\n" +
            "  if(uSwap > 0.5) dir = -dir;\n" +
            "  float sx = clamp(eu + dir * mag, 0.0, 1.0);\n" +
            "  gl_FragColor = texture2D(sTex, tx(vec2(sx, uv.y)));\n" +
            "}\n"
    }
}
