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
import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.webkit.WebView
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

/**
 * Entry point: walks the permission gauntlet (notifications, RECORD_AUDIO, draw-
 * over-other-apps, the AccessibilityService), fires the MediaProjection consent,
 * then starts the cast service. Hosts the same control web UI the desktop serves,
 * embedded in a WebView (no external browser), and a "minimize to balloon" button.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var status: TextView
    private lateinit var btnCast: Button
    private lateinit var btnFile: Button
    private lateinit var btnMin: Button
    private lateinit var web: WebView

    private val fileLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) {
                runCatching {
                    contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                promptAccessibilityIfNeeded()
                BsdrService.startFile(this, uri)
                onCasting()
            }
        }

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { res ->
            if (res.resultCode == RESULT_OK && res.data != null) {
                BsdrService.startCast(this, res.resultCode, res.data!!)
                onCasting()
            } else status.text = getString(R.string.cast_start)
        }

    private val permLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { beginProjection() }

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.status)
        btnCast = findViewById(R.id.btnCast)
        btnFile = findViewById(R.id.btnFile)
        btnMin = findViewById(R.id.btnMinimize)
        web = findViewById(R.id.web)
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true

        btnCast.setOnClickListener {
            if (NativeBridge.running) { BsdrService.stop(this); onStopped() }
            else requestPermsThenCast()
        }
        btnFile.setOnClickListener {
            if (NativeBridge.running) { BsdrService.stop(this); onStopped() }
            else fileLauncher.launch(arrayOf("video/*"))   // pick a local video to stream
        }
        btnMin.setOnClickListener {
            if (!ensureOverlayPermission()) return@setOnClickListener
            BsdrService.showBubble(this)
            moveTaskToBack(true)        // leave the app; the balloon keeps control
        }
        if (NativeBridge.running) onCasting() else onStopped()
    }

    private fun requestPermsThenCast() {
        val needed = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            needed += Manifest.permission.POST_NOTIFICATIONS
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) beginProjection() else permLauncher.launch(missing.toTypedArray())
    }

    private fun beginProjection() {
        promptAccessibilityIfNeeded()
        val mpm = getSystemService(MediaProjectionManager::class.java)
        projectionLauncher.launch(mpm.createScreenCaptureIntent())
    }

    /** The balloon needs "draw over other apps"; send the user to grant it once. */
    private fun ensureOverlayPermission(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this)) return true
        startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
            Uri.parse("package:$packageName")))
        return false
    }

    /** Input control needs the AccessibilityService enabled; nudge the user there. */
    private fun promptAccessibilityIfNeeded() {
        if (BsdrAccessibilityService.instance == null)
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
    }

    private fun onCasting() {
        status.text = getString(R.string.cast_stop)
        btnCast.text = getString(R.string.cast_stop)
        btnMin.isEnabled = true
        web.loadUrl("http://127.0.0.1:${NativeBridge.UI_PORT}/")
    }

    private fun onStopped() {
        status.text = getString(R.string.cast_start)
        btnCast.text = getString(R.string.cast_start)
        btnMin.isEnabled = false
        web.loadData(
            "<html><body style='font-family:sans-serif;padding:24px'>" +
                "<h3>bsdrX</h3><p>Tap <b>Start casting</b> to share this screen to your headset.</p>" +
                "</body></html>", "text/html", "utf-8")
    }
}
