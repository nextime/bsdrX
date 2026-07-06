/* X11 top-level window enumeration via _NET_CLIENT_LIST (EWMH). */
#include "bsdr/winlist.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *text_prop(Display *d, Window w, Atom prop) {
    if (prop == None) return NULL;
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, prop, 0, 256, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        char *s = malloc(n + 1); if (s) { memcpy(s, data, n); s[n] = 0; }
        XFree(data); return s;
    }
    return NULL;
}

/* Enumerate connected monitors via `xrandr --listmonitors` (same tool the screen-blank uses, so no
 * new lib dependency). Each line looks like: ` 0: +*DP-1 2560/597x1440/336+0+0  DP-1` — parse the
 * geometry token WIDTH/mmWx HEIGHT/mmH +X +Y into a capture region. */
int bsdr_list_monitors(const char *display, bsdr_window *out, int max) {
    if (!out || max <= 0) return 0;
    char cmd[256];
    /* honor an explicit DISPLAY so it matches the capture target */
    if (display && *display) snprintf(cmd, sizeof cmd, "DISPLAY=%s xrandr --listmonitors 2>/dev/null", display);
    else                     snprintf(cmd, sizeof cmd, "xrandr --listmonitors 2>/dev/null");
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char line[512]; int count = 0;
    while (fgets(line, sizeof line, f) && count < max) {
        char name[128], geo[160];
        if (sscanf(line, " %*d: %127s %159s", name, geo) != 2) continue;   /* skip the "Monitors: N" header */
        int w = 0, h = 0, x = 0, y = 0, wm = 0, hm = 0;
        if (sscanf(geo, "%d/%dx%d/%d%d%d", &w, &wm, &h, &hm, &x, &y) != 6) continue;
        if (w <= 0 || h <= 0) continue;
        const char *nm = name; while (*nm == '+' || *nm == '*') nm++;      /* strip primary/enabled marks */
        bsdr_window *o = &out[count++];
        o->id = 0; o->x = x; o->y = y; o->w = w; o->h = h;
        snprintf(o->title, sizeof o->title, "%s", nm);
    }
    pclose(f);
    return count;
}

int bsdr_list_windows(const char *display, bsdr_window *out, int max) {
    Display *d = XOpenDisplay((display && *display) ? display : NULL);
    if (!d) return 0;
    Window root = DefaultRootWindow(d);
    Atom clients  = XInternAtom(d, "_NET_CLIENT_LIST", True);
    Atom wmname   = XInternAtom(d, "_NET_WM_NAME", True);
    int count = 0;
    if (clients != None) {
        Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
        if (XGetWindowProperty(d, root, clients, 0, 1024, False, XA_WINDOW,
                               &type, &fmt, &n, &after, &data) == Success && data) {
            Window *wins = (Window *)data;
            for (unsigned long i = 0; i < n && count < max; i++) {
                Window w = wins[i];
                XWindowAttributes a;
                if (!XGetWindowAttributes(d, w, &a) || a.map_state != IsViewable) continue;
                if (a.width < 32 || a.height < 32) continue;
                int ax = 0, ay = 0; Window child;
                XTranslateCoordinates(d, w, root, 0, 0, &ax, &ay, &child);
                char *t = text_prop(d, w, wmname);
                if (!t) t = text_prop(d, w, XA_WM_NAME);
                bsdr_window *o = &out[count++];
                o->id = (unsigned long)w; o->x = ax; o->y = ay; o->w = a.width; o->h = a.height;
                snprintf(o->title, sizeof(o->title), "%s", t ? t : "(untitled)");
                free(t);
            }
            XFree(data);
        }
    }
    XCloseDisplay(d);
    return count;
}
