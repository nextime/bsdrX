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
import android.content.Context
import android.graphics.PixelFormat
import android.os.Build
import android.view.Gravity
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.webkit.WebView
import android.widget.Button

/**
 * The floating "balloon": a system overlay (TYPE_APPLICATION_OVERLAY) that keeps
 * the cast controllable while the user navigates other apps. Collapsed it's a
 * small draggable bubble; tapped it expands to the same control web UI the app
 * embeds, plus a quick Stop. Requires the "draw over other apps" permission.
 */
class BubbleOverlay(private val ctx: Context, private val onStop: () -> Unit) {

    /** When computer control is armed, a tap on the bubble drives the voice cycle
     *  (start/stop/confirm) instead of expanding; a long-press still opens the panel. */
    @Volatile var voiceActive = false
    var onVoiceTap: (() -> Unit)? = null

    private val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
    private var collapsed: View? = null
    private var expanded: View? = null
    private val overlayType =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        else @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE

    private fun params(w: Int, h: Int) = WindowManager.LayoutParams(
        w, h, overlayType,
        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
        PixelFormat.TRANSLUCENT
    ).apply { gravity = Gravity.TOP or Gravity.START; x = 24; y = 160 }

    fun show() {
        if (collapsed != null || expanded != null) return
        showCollapsed()
    }

    @SuppressLint("ClickableViewAccessibility", "InflateParams")
    private fun showCollapsed() {
        removeAll()
        val view = LayoutInflater.from(ctx).inflate(R.layout.bubble_collapsed, null)
        val lp = params(WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT)
        val icon = view.findViewById<View>(R.id.bubbleIcon)
        // tap: drive the voice cycle when armed, else open the control panel.
        icon.setOnTouchListener(dragToggle(lp) {
            if (voiceActive && onVoiceTap != null) onVoiceTap?.invoke() else showExpanded()
        })
        icon.setOnLongClickListener { showExpanded(); true }   // long-press always opens the panel
        wm.addView(view, lp)
        collapsed = view
    }

    @SuppressLint("SetJavaScriptEnabled", "InflateParams")
    private fun showExpanded() {
        removeAll()
        val view = LayoutInflater.from(ctx).inflate(R.layout.bubble_expanded, null)
        val lp = params(dp(320), dp(440))
        view.findViewById<WebView>(R.id.bubbleWeb).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            loadUrl("http://127.0.0.1:${NativeBridge.UI_PORT}/")
        }
        view.findViewById<Button>(R.id.bubbleStop).setOnClickListener { onStop() }
        view.findViewById<Button>(R.id.bubbleCollapse).setOnClickListener { showCollapsed() }
        // the header doubles as a drag handle
        view.findViewById<View>(R.id.bubbleHeader).setOnTouchListener(dragToggle(lp, null))
        wm.addView(view, lp)
        expanded = view
    }

    /** A touch listener that drags the window and, on a tap (no drag), runs onTap. */
    private fun dragToggle(lp: WindowManager.LayoutParams, onTap: (() -> Unit)?) =
        object : View.OnTouchListener {
            private var ix = 0; private var iy = 0
            private var tx = 0f; private var ty = 0f
            private var moved = false
            override fun onTouch(v: View, e: MotionEvent): Boolean {
                when (e.action) {
                    MotionEvent.ACTION_DOWN -> {
                        ix = lp.x; iy = lp.y; tx = e.rawX; ty = e.rawY; moved = false
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val dx = (e.rawX - tx).toInt(); val dy = (e.rawY - ty).toInt()
                        if (Math.abs(dx) > 8 || Math.abs(dy) > 8) moved = true
                        lp.x = ix + dx; lp.y = iy + dy
                        runCatching { wm.updateViewLayout(currentView(), lp) }
                    }
                    MotionEvent.ACTION_UP -> if (!moved) onTap?.invoke()
                }
                return true
            }
        }

    private fun currentView(): View? = expanded ?: collapsed

    private fun removeAll() {
        collapsed?.let { runCatching { wm.removeView(it) } }; collapsed = null
        expanded?.let { runCatching { wm.removeView(it) } }; expanded = null
    }

    fun hide() = removeAll()

    private fun dp(v: Int): Int = (v * ctx.resources.displayMetrics.density).toInt()
}
