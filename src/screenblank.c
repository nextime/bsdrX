/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* screenblank.c — privacy screen-blank (gamma-to-black at scanout). See screenblank.h. */
#include "bsdr/screenblank.h"
#include "bsdr/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
/* ---------------------------------------------------------------- Windows: gamma ramp */
#include <windows.h>
/* SetDeviceGammaRamp on the primary adapter's DC. The ramp is the GPU scanout LUT, applied after the
 * framebuffer gdigrab reads, so capture keeps full content. NB: Windows 10/11 may clamp ramps far from
 * linear unless HKLM\...\ICM\GdiICMGammaRange allows the full range; then the blank is partial. */
static WORD  g_saved[3][256];
static int   g_have_saved = 0;
void bsdr_screen_blank(int on) {
    HDC dc = GetDC(NULL);
    if (!dc) return;
    if (on) {
        if (!g_have_saved && GetDeviceGammaRamp(dc, g_saved)) g_have_saved = 1;
        WORD black[3][256];
        memset(black, 0, sizeof black);
        if (!SetDeviceGammaRamp(dc, black))
            BSDR_WARN("bsdr.blank", "SetDeviceGammaRamp failed (Windows may clamp gamma; see GdiICMGammaRange)");
    } else if (g_have_saved) {
        SetDeviceGammaRamp(dc, g_saved);
    }
    ReleaseDC(NULL, dc);
}

#elif defined(__APPLE__)
/* ---------------------------------------------------------------- macOS: CoreGraphics gamma */
#include <CoreGraphics/CoreGraphics.h>
/* Pin each active display's transfer function to black (min=max=0). The LUT is applied at scanout, so
 * avfoundation screen capture is unaffected; CGDisplayRestoreColorSyncSettings puts it all back. */
void bsdr_screen_blank(int on) {
    CGDirectDisplayID ids[16]; uint32_t n = 0;
    if (CGGetActiveDisplayList(16, ids, &n) != kCGErrorSuccess) return;
    if (on) {
        for (uint32_t i = 0; i < n; i++)
            CGSetDisplayTransferByFormula(ids[i], 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    } else {
        CGDisplayRestoreColorSyncSettings();
    }
}

#elif defined(__linux__) && !defined(__ANDROID__)
/* ---------------------------------------------------------------- Linux: Wayland gamma or X11 xrandr */
#if defined(BSDR_HAVE_WAYLAND_GAMMA)
int  bsdr_wl_gamma_blank(int on);   /* screenblank_wayland.c: 0 = handled, -1 = unavailable */
#endif
void bsdr_screen_blank(int on) {
#if defined(BSDR_HAVE_WAYLAND_GAMMA)
    if (getenv("WAYLAND_DISPLAY")) {
        if (bsdr_wl_gamma_blank(on) == 0) return;         /* wlroots compositor handled it */
        BSDR_WARN("bsdr.blank", "Wayland gamma-control unavailable (not a wlroots compositor?) — "
                                "privacy screen-blank not applied");
        return;
    }
#endif
    if (!getenv("DISPLAY")) return;
    /* X11: xrandr --brightness is a CRTC gamma ramp; applied to every connected output. */
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "for o in $(xrandr -q 2>/dev/null | grep ' connected' | cut -d' ' -f1); do "
        "xrandr --output \"$o\" --brightness %s 2>/dev/null; done",
        on ? "0" : "1");
    if (system(cmd) != 0) { /* best-effort */ }
}

#else
/* ---------------------------------------------------------------- other (Android): no-op */
void bsdr_screen_blank(int on) { (void)on; }
#endif
