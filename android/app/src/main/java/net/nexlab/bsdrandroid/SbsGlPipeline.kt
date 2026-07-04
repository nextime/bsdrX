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

    private lateinit var surfaceTexture: SurfaceTexture
    /** MediaProjection's VirtualDisplay renders here; valid after construction. */
    lateinit var inputSurface: Surface
        private set

    // live SBS params (read on the GL thread each frame)
    @Volatile private var pMode = 1
    @Volatile private var pAmp = 0.35f * 0.04f
    @Volatile private var pC0 = 0.5f
    @Volatile private var pSwap = 0f

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

    /** Update the SBS parameters (mode 0/1/2, deepness 0..100, convergence -50..50, swap 0/1). */
    fun setParams(mode: Int, deepness: Int, convergence: Int, swap: Int) {
        pMode = if (mode < 0) 0 else mode
        pAmp = (deepness.coerceIn(0, 100) / 100f) * 0.04f
        pC0 = 0.5f - convergence.coerceIn(-50, 50) / 100f
        pSwap = if (swap != 0) 1f else 0f
    }

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

        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        texId = tex[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        surfaceTexture = SurfaceTexture(texId).apply {
            setDefaultBufferSize(srcWidth, height)
            setOnFrameAvailableListener({ drawFrame() }, glHandler)
        }
        inputSurface = Surface(surfaceTexture)
    }

    private fun drawFrame() {
        try {
            surfaceTexture.updateTexImage()
            surfaceTexture.getTransformMatrix(texMatrix)

            GLES20.glViewport(0, 0, outWidth, height)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
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
            GLES20.glUniform1i(uMode, pMode)
            GLES20.glUniform1f(uAmp, pAmp)
            GLES20.glUniform1f(uC0, pC0)
            GLES20.glUniform1f(uSwap, pSwap)

            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

            GLES20.glDisableVertexAttribArray(aPos)
            GLES20.glDisableVertexAttribArray(aTex)

            EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, surfaceTexture.timestamp)
            EGL14.eglSwapBuffers(eglDisplay, eglSurface)
        } catch (e: Exception) {
            Log.w(TAG, "SBS draw failed: ${e.message}")
        }
    }

    fun release() {
        try {
            val done = CountDownLatch(1)
            glHandler.post {
                try {
                    if (::surfaceTexture.isInitialized) surfaceTexture.release()
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
            "vec2 tx(vec2 uv){ return (uTexMatrix * vec4(uv, 0.0, 1.0)).xy; }\n" +
            "void main(){\n" +
            "  vec2 uv = vTex;\n" +
            "  if(uMode == 0){ gl_FragColor = texture2D(sTex, tx(uv)); return; }\n" +
            "  bool left = uv.x < 0.5;\n" +
            "  float eu = left ? uv.x * 2.0 : (uv.x - 0.5) * 2.0;\n" +
            "  float lum = dot(texture2D(sTex, tx(vec2(eu, uv.y))).rgb, vec3(0.299,0.587,0.114));\n" +
            // vTex.y = 0 is the bottom of the output image -> near; top -> far (matches threed.c).
            "  float d = clamp(0.6 * (1.0 - uv.y) + 0.4 * lum, 0.0, 1.0);\n" +
            "  float mag = uAmp * (d - uC0) * 0.5;\n" +
            "  float dir = left ? 1.0 : -1.0;\n" +
            "  if(uSwap > 0.5) dir = -dir;\n" +
            "  float sx = clamp(eu + dir * mag, 0.0, 1.0);\n" +
            "  gl_FragColor = texture2D(sTex, tx(vec2(sx, uv.y)));\n" +
            "}\n"
    }
}
