/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager

/**
 * The foreground service: owns the native agent lifecycle, the MediaProjection,
 * the screen encoder, the audio I/O and the floating balloon. It survives the
 * app being backgrounded so the cast keeps running while the user navigates other
 * apps — the balloon is the control surface during that time.
 */
class BsdrService : Service() {

    private var projection: MediaProjection? = null
    private var capture: ScreenCapture? = null
    private var audio: AudioIO? = null
    private var bubble: BubbleOverlay? = null
    private var voiceMic: VoiceMic? = null
    private var fileSource: FileSource? = null
    @Volatile private var voiceState = NativeBridge.Voice.IDLE
    private val main = Handler(Looper.getMainLooper())

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopEverything(); stopSelf(); return START_NOT_STICKY }
            ACTION_SHOW_BUBBLE -> { bubble?.show(); return START_STICKY }
            ACTION_HIDE_BUBBLE -> { bubble?.hide(); return START_STICKY }
            ACTION_START_FILE -> return startFileCast(intent)
        }

        val resultCode = intent?.getIntExtra(EXTRA_RESULT_CODE, 0) ?: 0
        val data = intent?.getParcelableExtra<Intent>(EXTRA_DATA)
        if (resultCode == 0 || data == null) { stopSelf(); return START_NOT_STICKY }

        startForegroundCompat()

        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val proj = mpm.getMediaProjection(resultCode, data).also { projection = it }
        proj.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() { stopEverything(); stopSelf() }
        }, Handler(Looper.getMainLooper()))

        val (w, h, dpi) = screenSpec()
        Log.i(TAG, "casting ${w}x$h @ ${dpi}dpi")

        NativeBridge.pairingListener = { code -> updateNotification(getString(R.string.pairing_code, code)) }
        NativeBridge.start(w, h, FPS, BITRATE)
        capture = ScreenCapture(proj, w, h, dpi, FPS, BITRATE).also { it.start() }
        audio = AudioIO(proj).also { runCatching { it.start() }.onFailure { e -> Log.w(TAG, "audio off", e) } }
        wireControls(Screenshotter(proj, w, h, dpi))   // bubble + voice + vision screenshot
        return START_STICKY
    }

    /** Stream a local video file instead of the live screen (no MediaProjection). */
    private fun startFileCast(intent: Intent): Int {
        val uri = intent.getParcelableExtra<android.net.Uri>(EXTRA_FILE_URI)
            ?: run { stopSelf(); return START_NOT_STICKY }
        startForegroundCompat(fileMode = true)
        val fs = FileSource(this, uri, 0, 0, FPS, BITRATE)
        if (!fs.probe()) {
            Log.w(TAG, "cannot read video file $uri")
            stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); return START_NOT_STICKY
        }
        Log.i(TAG, "streaming file ${fs.srcWidth}x${fs.srcHeight}")
        NativeBridge.pairingListener = { code -> updateNotification(getString(R.string.pairing_code, code)) }
        NativeBridge.start(fs.srcWidth, fs.srcHeight, FPS, BITRATE)
        fs.start(); fileSource = fs
        wireControls(null)      // no projection -> no vision screenshot in file mode
        return START_STICKY
    }

    /** Shared control wiring: the floating bubble, device-mic voice control, and (when
     *  a Screenshotter is available) the vision screenshot tool. */
    private fun wireControls(shooter: Screenshotter?) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M ||
            android.provider.Settings.canDrawOverlays(this)) {
            bubble = BubbleOverlay(this) { stopEverything(); stopSelf() }.also { b ->
                b.onVoiceTap = {
                    when (voiceState) {
                        NativeBridge.Voice.IDLE      -> NativeBridge.nativeVoiceTrigger()
                        NativeBridge.Voice.LISTENING -> NativeBridge.nativeVoiceStop()
                        NativeBridge.Voice.CONFIRM   -> NativeBridge.nativeVoiceConfirm(true)  // tap = Send
                    }
                }
                b.show()
            }
        }
        NativeBridge.compctlActiveListener = { active -> main.post {
            if (active) { if (voiceMic == null) voiceMic = VoiceMic().also { it.start() }; bubble?.voiceActive = true }
            else { voiceMic?.stop(); voiceMic = null; bubble?.voiceActive = false }
        } }
        NativeBridge.voiceStateListener = { st -> voiceState = st }
        NativeBridge.voiceFeedbackListener = { msg -> Log.i(TAG, "voice: $msg") }
        NativeBridge.screenshotProvider = shooter?.let { s -> { maxDim: Int -> s.grab(maxDim) } }
    }

    private fun screenSpec(): Triple<Int, Int, Int> {
        val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val b = wm.currentWindowMetrics.bounds
            Triple(b.width() and 1.inv(), b.height() and 1.inv(),  // even dims for the encoder
                resources.configuration.densityDpi)
        } else {
            val dm = DisplayMetrics()
            @Suppress("DEPRECATION") wm.defaultDisplay.getRealMetrics(dm)
            Triple(dm.widthPixels and 1.inv(), dm.heightPixels and 1.inv(), dm.densityDpi)
        }
    }

    private fun stopEverything() {
        voiceMic?.stop(); voiceMic = null
        fileSource?.stop(); fileSource = null
        capture?.stop(); capture = null
        audio?.stop(); audio = null
        NativeBridge.stop()
        NativeBridge.pairingListener = null
        NativeBridge.compctlActiveListener = null
        NativeBridge.voiceStateListener = null
        NativeBridge.voiceFeedbackListener = null
        NativeBridge.screenshotProvider = null
        bubble?.hide(); bubble = null
        try { projection?.stop() } catch (_: Exception) {}
        projection = null
    }

    override fun onDestroy() { stopEverything(); super.onDestroy() }

    // ---- notification --------------------------------------------------------
    private fun startForegroundCompat(fileMode: Boolean = false) {
        val mgr = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CH, "bsdrX cast", NotificationManager.IMPORTANCE_LOW))
        }
        val notif = buildNotification(getString(R.string.notif_text))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // file streaming isn't a screen capture -> declare dataSync, not mediaProjection
            val type = if (fileMode) ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                       else ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            startForeground(NID, notif, type)
        } else {
            startForeground(NID, notif)
        }
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val stop = PendingIntent.getService(this, 1,
            Intent(this, BsdrService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val b = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            Notification.Builder(this, CH) else @Suppress("DEPRECATION") Notification.Builder(this)
        return b.setContentTitle(getString(R.string.notif_title))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_bubble)
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.cast_stop), stop).build())
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java).notify(NID, buildNotification(text))
    }

    companion object {
        private const val TAG = "bsdr.service"
        private const val CH = "bsdr.cast"
        private const val NID = 1
        private const val FPS = 30
        private const val BITRATE = 8_000_000
        const val EXTRA_RESULT_CODE = "rc"
        const val EXTRA_DATA = "data"
        const val EXTRA_FILE_URI = "fileUri"
        const val ACTION_STOP = "net.nexlab.bsdrandroid.STOP"
        const val ACTION_SHOW_BUBBLE = "net.nexlab.bsdrandroid.SHOW_BUBBLE"
        const val ACTION_HIDE_BUBBLE = "net.nexlab.bsdrandroid.HIDE_BUBBLE"
        const val ACTION_START_FILE = "net.nexlab.bsdrandroid.START_FILE"

        fun startCast(ctx: Context, resultCode: Int, data: Intent) {
            val i = Intent(ctx, BsdrService::class.java)
                .putExtra(EXTRA_RESULT_CODE, resultCode).putExtra(EXTRA_DATA, data)
            ctx.startForegroundService(i)
        }
        fun startFile(ctx: Context, uri: android.net.Uri) {
            val i = Intent(ctx, BsdrService::class.java).setAction(ACTION_START_FILE)
                .putExtra(EXTRA_FILE_URI, uri)
            ctx.startForegroundService(i)
        }
        fun stop(ctx: Context) =
            ctx.startService(Intent(ctx, BsdrService::class.java).setAction(ACTION_STOP))
        fun showBubble(ctx: Context) =
            ctx.startService(Intent(ctx, BsdrService::class.java).setAction(ACTION_SHOW_BUBBLE))
    }
}
