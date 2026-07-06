/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* winlist_macos.c — on-screen window + monitor enumeration for the web UI's single-window picker.
 *
 * Uses CoreGraphics' CGWindowList (plain C over CoreFoundation, no Obj-C). The web UI turns the picked
 * window's geometry into an x/y/w/h region; capture.c software-crops the avfoundation full-display grab
 * to it (avfoundation can't crop at grab time). Window titles need the Screen Recording permission on
 * 10.15+ (the geometry+owner do not) — bsdrX already prompts for that grant on first capture. */
#include "bsdr/winlist.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

static void cfstr_to_utf8(CFStringRef s, char *out, int cap) {
    out[0] = '\0';
    if (s) CFStringGetCString(s, out, cap, kCFStringEncodingUTF8);
}

int bsdr_list_windows(const char *display, bsdr_window *out, int max) {
    (void)display;
    if (!out || max <= 0) return 0;
    CFArrayRef wins = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!wins) return 0;
    int n = 0;
    CFIndex cnt = CFArrayGetCount(wins);
    for (CFIndex i = 0; i < cnt && n < max; i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(wins, i);
        if (!d) continue;
        /* Only normal application windows (layer 0) — skip the menu bar, Dock, shadows, etc. */
        CFNumberRef layer = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowLayer);
        int lv = 0; if (layer) CFNumberGetValue(layer, kCFNumberIntType, &lv);
        if (lv != 0) continue;
        CFDictionaryRef bd = (CFDictionaryRef)CFDictionaryGetValue(d, kCGWindowBounds);
        CGRect r;
        if (!bd || !CGRectMakeWithDictionaryRepresentation(bd, &r)) continue;
        if (r.size.width < 2 || r.size.height < 2) continue;   /* skip degenerate windows */

        char owner[128] = "", wname[160] = "";
        cfstr_to_utf8((CFStringRef)CFDictionaryGetValue(d, kCGWindowOwnerName), owner, sizeof owner);
        cfstr_to_utf8((CFStringRef)CFDictionaryGetValue(d, kCGWindowName), wname, sizeof wname);
        CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowNumber);
        long wid = 0; if (num) CFNumberGetValue(num, kCFNumberLongType, &wid);

        bsdr_window *o = &out[n++];
        o->id = (unsigned long)wid;
        o->x = (int)r.origin.x;    o->y = (int)r.origin.y;
        o->w = (int)r.size.width;  o->h = (int)r.size.height;
        if (owner[0] && wname[0]) snprintf(o->title, sizeof o->title, "%s \xe2\x80\x94 %s", owner, wname);
        else                      snprintf(o->title, sizeof o->title, "%s", owner[0] ? owner : (wname[0] ? wname : "(untitled)"));
    }
    CFRelease(wins);
    return n;
}

int bsdr_list_monitors(const char *display, bsdr_window *out, int max) {
    (void)display;
    if (!out || max <= 0) return 0;
    CGDirectDisplayID ids[16];
    uint32_t cnt = 0;
    if (CGGetActiveDisplayList(16, ids, &cnt) != kCGErrorSuccess) return 0;
    int n = 0;
    for (uint32_t i = 0; i < cnt && n < max; i++) {
        CGRect r = CGDisplayBounds(ids[i]);
        bsdr_window *o = &out[n++];
        o->id = (unsigned long)ids[i];
        o->x = (int)r.origin.x;    o->y = (int)r.origin.y;
        o->w = (int)r.size.width;  o->h = (int)r.size.height;
        snprintf(o->title, sizeof o->title, "Display %u (%dx%d)", (unsigned)(i + 1), o->w, o->h);
    }
    return n;
}
