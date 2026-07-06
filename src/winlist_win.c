/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
/* Windows window enumeration (the Win32 counterpart of winlist.c). Lists
 * visible, titled top-level windows with their screen geometry so the web UI
 * can stream a single window (the capture region is derived from x/y/w/h, fed
 * to gdigrab). */
#ifdef _WIN32
#include "bsdr/winlist.h"
#include <windows.h>
#include <string.h>

struct enum_ctx { bsdr_window *out; int max; int count; };

static BOOL CALLBACK enum_cb(HWND hwnd, LPARAM lp) {
    struct enum_ctx *c = (struct enum_ctx *)lp;
    if (c->count >= c->max) return FALSE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    /* skip tool windows and zero-size/minimized windows */
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (IsIconic(hwnd)) return TRUE;

    wchar_t wtitle[256];
    int wl = GetWindowTextW(hwnd, wtitle, 256);
    if (wl <= 0) return TRUE;                     /* untitled: skip */

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 1 || h <= 1) return TRUE;

    bsdr_window *o = &c->out[c->count];
    o->id = (unsigned long)(uintptr_t)hwnd;
    o->x = r.left; o->y = r.top; o->w = w; o->h = h;
    WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, o->title, (int)sizeof(o->title) - 1, NULL, NULL);
    o->title[sizeof(o->title) - 1] = '\0';
    c->count++;
    return TRUE;
}

int bsdr_list_windows(const char *display, bsdr_window *out, int max) {
    (void)display;   /* no X display on Windows */
    struct enum_ctx c = { out, max, 0 };
    EnumWindows(enum_cb, (LPARAM)&c);
    return c.count;
}

/* Each connected monitor as a capture region (gdigrab crops to x/y/w/h). */
struct mon_ctx { bsdr_window *out; int max, count; };
static BOOL CALLBACK mon_cb(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r;
    struct mon_ctx *c = (struct mon_ctx *)lp;
    if (c->count >= c->max) return FALSE;
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(mon, (MONITORINFO *)&mi)) return TRUE;
    bsdr_window *o = &c->out[c->count++];
    o->id = 0;
    o->x = mi.rcMonitor.left; o->y = mi.rcMonitor.top;
    o->w = mi.rcMonitor.right - mi.rcMonitor.left;
    o->h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    snprintf(o->title, sizeof o->title, "%s%s", mi.szDevice,
             (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
    return TRUE;
}
int bsdr_list_monitors(const char *display, bsdr_window *out, int max) {
    (void)display;
    if (!out || max <= 0) return 0;
    struct mon_ctx c = { out, max, 0 };
    EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)&c);
    return c.count;
}
#endif /* _WIN32 */
