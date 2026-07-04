/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.io.ByteArrayOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * On-demand single-frame screen grab for the voice model's vision tool. Uses the
 * live MediaProjection to spin up a short-lived VirtualDisplay -> ImageReader,
 * captures one frame, and JPEG-encodes it (scaled to maxDim on the long side).
 * grab() blocks (the native voice worker calls it synchronously) up to ~2 s.
 */
class Screenshotter(
    private val projection: MediaProjection,
    private val w: Int, private val h: Int, private val dpi: Int,
) {
    fun grab(maxDim: Int): ByteArray? {
        val reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2)
        val ht = HandlerThread("bsdr-shot").apply { start() }
        val handler = Handler(ht.looper)
        val latch = CountDownLatch(1)
        var result: ByteArray? = null   // published to the caller via latch happens-before

        reader.setOnImageAvailableListener({ r ->
            if (latch.count == 0L) { r.acquireLatestImage()?.close(); return@setOnImageAvailableListener }
            val img = r.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val plane = img.planes[0]
                val pixStride = plane.pixelStride
                val rowStride = plane.rowStride
                // ImageReader rows are padded to rowStride; make a bitmap that wide, then crop.
                val padded = Bitmap.createBitmap(rowStride / pixStride, h, Bitmap.Config.ARGB_8888)
                padded.copyPixelsFromBuffer(plane.buffer)
                var bmp = Bitmap.createBitmap(padded, 0, 0, w, h)
                if (w > maxDim || h > maxDim) {
                    val s = if (w >= h) maxDim.toFloat() / w else maxDim.toFloat() / h
                    bmp = Bitmap.createScaledBitmap(bmp, (w * s).toInt().coerceAtLeast(2),
                        (h * s).toInt().coerceAtLeast(2), true)
                }
                val bos = ByteArrayOutputStream()
                bmp.compress(Bitmap.CompressFormat.JPEG, 80, bos)
                result = bos.toByteArray()
            } catch (e: Exception) {
                Log.w(TAG, "grab", e)
            } finally {
                img.close(); latch.countDown()
            }
        }, handler)

        val vd = try {
            projection.createVirtualDisplay("bsdr-shot", w, h, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, reader.surface, null, handler)
        } catch (e: Exception) { Log.w(TAG, "virtual display", e); null }

        try { latch.await(2, TimeUnit.SECONDS) } catch (_: InterruptedException) {}
        try { vd?.release() } catch (_: Exception) {}
        reader.close(); ht.quitSafely()
        return result
    }

    companion object { private const val TAG = "bsdr.shot" }
}
