/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
package net.nexlab.bsdrandroid

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.os.SystemClock
import android.util.Log
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo

/**
 * The input actuator: turns the headset's marshalled input commands (from native
 * via NativeBridge.inputListener) into Accessibility gestures and text. This is
 * the non-root ceiling — coarse taps/drags/scroll/text, not a pixel-precise
 * pointer; unmapped events are dropped on the native side (see inject_android.c).
 */
class BsdrAccessibilityService : AccessibilityService() {

    private var cx = 0; private var cy = 0      // last cursor position (px)
    private var downX = 0; private var downY = 0
    private var downAt = 0L

    override fun onServiceConnected() {
        instance = this
        NativeBridge.inputListener = { kind, a, b -> onInput(kind, a, b) }
        Log.i(TAG, "accessibility connected; input sink active")
    }

    private fun onInput(kind: Int, a: Int, b: Int) {
        when (kind) {
            NativeBridge.Input.MOVE -> { cx = a; cy = b }
            NativeBridge.Input.TAP  -> tap(a, b)
            NativeBridge.Input.DOWN -> { downX = a; downY = b; downAt = SystemClock.uptimeMillis() }
            NativeBridge.Input.UP   -> dragOrTap(a, b)
            NativeBridge.Input.SCROLL -> scroll(a, b)
            NativeBridge.Input.TEXT -> commitText(a)
            NativeBridge.Input.KEY  -> navKey(a)
        }
    }

    private fun stroke(p: Path, dur: Long) {
        val g = GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(p, 0, dur.coerceIn(1, 60000)))
            .build()
        dispatchGesture(g, null, null)
    }

    private fun tap(x: Int, y: Int) {
        val p = Path().apply { moveTo(x.toFloat(), y.toFloat()); lineTo(x + 1f, y + 1f) }
        stroke(p, 40)
    }

    private fun dragOrTap(x: Int, y: Int) {
        val dist = Math.hypot((x - downX).toDouble(), (y - downY).toDouble())
        if (dist < 12) { tap(x, y); return }
        val p = Path().apply { moveTo(downX.toFloat(), downY.toFloat()); lineTo(x.toFloat(), y.toFloat()) }
        stroke(p, (SystemClock.uptimeMillis() - downAt).coerceIn(40, 1500))
    }

    private fun scroll(dx: Int, dy: Int) {
        // map a wheel notch to a swipe from the current cursor position
        val p = Path().apply {
            moveTo(cx.toFloat(), cy.toFloat())
            lineTo((cx - dx * 40).toFloat(), (cy + dy * 120).toFloat())
        }
        stroke(p, 120)
    }

    /** Best-effort text: set/append on the focused editable node. */
    private fun commitText(codepoint: Int) {
        val node = findFocusedEditable() ?: return
        val cur = node.text?.toString() ?: ""
        val args = android.os.Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,
                cur + String(Character.toChars(codepoint)))
        }
        node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)
    }

    private fun findFocusedEditable(): AccessibilityNodeInfo? {
        val root = rootInActiveWindow ?: return null
        return root.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?: root.findFocus(AccessibilityNodeInfo.FOCUS_ACCESSIBILITY)
    }

    private fun navKey(akey: Int) {
        when (akey) {
            4   -> performGlobalAction(GLOBAL_ACTION_BACK)      // BACK
            3   -> performGlobalAction(GLOBAL_ACTION_HOME)      // HOME
            else -> Log.d(TAG, "navKey $akey (no global action)")
        }
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}
    override fun onDestroy() {
        if (instance === this) { instance = null; NativeBridge.inputListener = null }
        super.onDestroy()
    }

    companion object {
        private const val TAG = "bsdr.inject"
        @Volatile var instance: BsdrAccessibilityService? = null
            private set
    }
}
