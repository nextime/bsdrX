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
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
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
    private val main = Handler(Looper.getMainLooper())
    /** True while we want the control UI shown; drives the reachability retry loop. */
    @Volatile private var wantUi = false
    /** We only send the user to Accessibility settings once per run (it's optional for streaming). */
    private var accessibilityAsked = false
    /** Permissions panel collapse state. null = follow default (auto-collapse once all required are
     *  granted); true/false once the user taps the header to override. */
    private var permsExpandedOverride: Boolean? = null

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

    /** For the permissions panel: just refresh the list after a grant (don't start casting). */
    private val grantLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { renderPermissions() }

    /** Pick the faceswap .onnx models and copy them into the app's internal dir (where they're
     *  reliably readable — adb-pushed files under Android/data are not). */
    private val fsModelLauncher =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (uris.isNullOrEmpty()) return@registerForActivityResult
            importFaceswapModels(uris)
        }

    private val fsModelNames = listOf("det_10g.onnx", "w600k_r50.onnx", "inswapper_128.onnx")

    private fun faceswapModelsPresent(): Int {
        val dir = java.io.File(filesDir, "faceswap")
        return fsModelNames.count { java.io.File(dir, it).length() > 0 }
    }

    private fun importFaceswapModels(uris: List<Uri>) {
        val dir = java.io.File(filesDir, "faceswap").apply { mkdirs() }
        status.text = "Installing faceswap models…"
        Thread {
            var ok = 0
            for (u in uris) {
                val name = queryDisplayName(u) ?: continue
                val target = fsModelNames.firstOrNull { name.equals(it, true) || name.contains(it.removeSuffix(".onnx"), true) }
                    ?: continue
                try {
                    contentResolver.openInputStream(u)?.use { ins ->
                        java.io.File(dir, target).outputStream().use { outs -> ins.copyTo(outs, 1 shl 20) }
                    }
                    ok++
                } catch (e: Exception) { android.util.Log.w("bsdr.main", "import $target: ${e.message}") }
            }
            main.post {
                status.text = if (NativeBridge.running) getString(R.string.cast_stop) else getString(R.string.cast_start)
                android.widget.Toast.makeText(this, "Installed $ok/${fsModelNames.size} faceswap models", android.widget.Toast.LENGTH_LONG).show()
                renderPermissions()
            }
        }.start()
    }

    private fun queryDisplayName(uri: Uri): String? =
        contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        }

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
        // The native web server comes up asynchronously after casting starts, so the
        // first load can race ahead of it (ERR_CONNECTION_REFUSED). Retry until it's up.
        web.webViewClient = object : WebViewClient() {
            override fun onReceivedError(v: WebView, req: WebResourceRequest?, err: WebResourceError?) {
                if (wantUi && req?.isForMainFrame == true) main.postDelayed({ if (wantUi) loadUi() }, 500)
            }
        }

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
        val needed = mutableListOf(Manifest.permission.RECORD_AUDIO,
            Manifest.permission.CAMERA)   // camera up front so the web-UI webcam source works instantly
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

    /** True if our AccessibilityService is enabled in system settings. Checks the authoritative
     *  Settings.Secure list (not just the live `instance`, which is briefly null right after our
     *  process starts even when the service is enabled) so we don't re-send the user to Settings
     *  when they've already granted it. */
    private fun isAccessibilityEnabled(): Boolean {
        if (BsdrAccessibilityService.instance != null) return true
        val flat = Settings.Secure.getString(
            contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) ?: return false
        val me = "$packageName/${BsdrAccessibilityService::class.java.name}"
        return flat.split(':').any { it.equals(me, ignoreCase = true) }
    }

    /** Input control (remote mouse/keyboard from the headset) needs the AccessibilityService. It's
     *  optional for plain screen streaming, so nudge the user there ONLY if it isn't already on and
     *  we haven't already asked this run — never interrupt a cast when it's already granted. */
    private fun promptAccessibilityIfNeeded() {
        if (isAccessibilityEnabled() || accessibilityAsked) return
        accessibilityAsked = true
        startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
    }

    override fun onResume() { super.onResume(); renderPermissions() }

    /** Show, in plain language, exactly which permissions bsdrX needs and whether each is granted.
     *  Each row is tappable to grant/enable the missing one. */
    private fun renderPermissions() {
        val box = findViewById<android.widget.LinearLayout>(R.id.perms) ?: return
        box.removeAllViews()
        data class Perm(val name: String, val why: String, val granted: Boolean, val grant: () -> Unit)
        val list = mutableListOf<Perm>()
        list += Perm("Microphone", "voice control + streaming audio",
            ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED,
            { grantLauncher.launch(arrayOf(Manifest.permission.RECORD_AUDIO)) })
        list += Perm("Camera", "webcam streaming",
            ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED,
            { grantLauncher.launch(arrayOf(Manifest.permission.CAMERA)) })
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            list += Perm("Notifications", "the ongoing-cast notification",
                ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED,
                { grantLauncher.launch(arrayOf(Manifest.permission.POST_NOTIFICATIONS)) })
        list += Perm("Display over other apps", "the floating control balloon",
            Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this),
            { startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName"))) })
        // Accessibility is optional — it doesn't count toward "all required granted" for auto-collapse.
        val optionalGranted = isAccessibilityEnabled()
        list += Perm("Accessibility service", "remote mouse/keyboard from the headset (optional)",
            optionalGranted,
            { startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)) })

        val allRequiredGranted = list.dropLast(1).all { it.granted }   // every row except the optional Accessibility one
        // Default: expanded while something still needs granting, auto-collapsed once all required are done.
        // The user can override either way by tapping the header.
        val expanded = permsExpandedOverride ?: !allRequiredGranted

        val hdr = TextView(this).apply {
            text = if (expanded)
                (if (allRequiredGranted) "▾ Permissions — all set (tap to hide)"
                 else "▾ Permissions — tap a red item to enable it:")
            else
                "▸ Permissions — all set (tap to show)"
            setPadding(0, 4, 0, 6); textSize = 13f; setTypeface(typeface, android.graphics.Typeface.BOLD)
            setTextColor(if (allRequiredGranted) 0xFF2E7D32.toInt() else 0xFF000000.toInt())
            setOnClickListener {
                permsExpandedOverride = !expanded
                renderPermissions()
            }
        }
        box.addView(hdr)
        if (!expanded) return   // collapsed: header only, the rest is hidden until tapped

        for (p in list) {
            val row = TextView(this).apply {
                text = "${if (p.granted) "✓" else "✗"}  ${p.name} — ${p.why}"
                setTextColor(if (p.granted) 0xFF2E7D32.toInt() else 0xFFC62828.toInt())
                setPadding(0, 6, 0, 6); textSize = 13f
                if (!p.granted) setOnClickListener { p.grant() }
            }
            box.addView(row)
        }
        val note = TextView(this).apply {
            text = "Screen capture is asked each time you Start casting — Android requires that per session."
            setPadding(0, 6, 0, 2); textSize = 11f; setTextColor(0xFF888888.toInt())
        }
        box.addView(note)

        val n = faceswapModelsPresent()
        val fsRow = TextView(this).apply {
            text = "${if (n == 3) "✓" else "⤓"}  Faceswap models: $n/3 installed — tap to install (pick det_10g / w600k_r50 / inswapper_128 .onnx)"
            setTextColor(if (n == 3) 0xFF2E7D32.toInt() else 0xFF1565C0.toInt())
            setPadding(0, 8, 0, 6); textSize = 12f
            setOnClickListener { fsModelLauncher.launch(arrayOf("*/*")) }
        }
        box.addView(fsRow)
    }

    private fun loadUi() { web.loadUrl("http://127.0.0.1:${NativeBridge.UI_PORT}/") }

    private fun onCasting() {
        status.text = getString(R.string.cast_stop)
        btnCast.text = getString(R.string.btn_cast_stop)
        btnMin.isEnabled = true
        wantUi = true
        loadUi()        // retried by the WebViewClient until the native server binds 8088
    }

    private fun onStopped() {
        wantUi = false
        status.text = getString(R.string.cast_start)
        btnCast.text = getString(R.string.btn_cast_start)
        btnMin.isEnabled = false
        web.loadData(
            "<html><body style='font-family:sans-serif;padding:24px'>" +
                "<h3>bsdrX</h3><p>Tap <b>Start casting</b> to share this screen to your headset.</p>" +
                "</body></html>", "text/html", "utf-8")
    }
}
