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
/* The native control-panel window + its lifecycle.
 *
 * In app-window mode the panel is a chromeless Chrome/Edge/Chromium `--app`
 * window launched as a TRACKED child process (dedicated user-data-dir, so the
 * browser can't hand the window to an already-running instance and vanish).
 *
 * - Windows: a system-tray icon is installed. Closing the window keeps the app
 *   running in the tray; a right-click menu offers "Open bsdrX" (reopen the
 *   window) and "Quit bsdrX" (which fires on_quit). Double-click reopens too.
 * - Linux: a StatusNotifier tray via AppIndicator IS used when one is available
 *   (loaded at runtime with dlopen — no build dependency); same Open/Quit menu.
 *   If no tray is available (e.g. a desktop without a StatusNotifier host, or a
 *   headless session), it falls back to close-to-quit.
 * - macOS (and the Linux no-tray fallback): closing the tracked window fires
 *   on_quit, i.e. closing the window exits the whole application.
 *
 * In plain-browser mode (app_mode=false) this just opens the default browser
 * once: no tray, no tracking, on_quit never fires. */
#ifndef BSDR_APPWINDOW_H
#define BSDR_APPWINDOW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct bsdr_appwindow bsdr_appwindow;

/* Open the control panel for the local UI on `port`. When `app_mode` is true a
 * chromeless app window is used and its lifecycle is managed (tray on Windows,
 * close-to-quit elsewhere); `on_quit(user)` is invoked when the user asks to
 * quit (tray "Quit", or the window closing on a platform without a tray).
 * Returns a handle to stop later, or NULL if nothing could be launched (the
 * caller keeps running headless; the panel is still reachable manually). */
bsdr_appwindow *bsdr_appwindow_start(uint16_t port, bool app_mode,
                                     void (*on_quit)(void *user), void *user);

/* Remove the tray icon, close the window if still open, join the helper thread
 * and free the handle. Safe to call with NULL. Does NOT fire on_quit. */
void bsdr_appwindow_stop(bsdr_appwindow *w);

#endif /* BSDR_APPWINDOW_H */
