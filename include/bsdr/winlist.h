/* List on-screen X11 windows (id + title + absolute geometry) so the web UI can
 * pick a single window to stream instead of the whole desktop. */
#ifndef BSDR_WINLIST_H
#define BSDR_WINLIST_H
typedef struct { unsigned long id; int x, y, w, h; char title[200]; } bsdr_window;
/* Fill out[] with up to `max` viewable top-level windows; returns the count. */
int bsdr_list_windows(const char *display, bsdr_window *out, int max);
#endif
