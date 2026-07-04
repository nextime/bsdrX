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
