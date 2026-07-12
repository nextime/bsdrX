/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* screenblank.h — privacy screen-blank.
 *
 * Black out the PHYSICAL monitor(s) while streaming, WITHOUT blacking the captured content (a bystander
 * at the PC sees nothing; the Quest still gets the full desktop). The trick is to blank at the output's
 * gamma LUT / scanout, which is applied AFTER the framebuffer the capture reads — so, unlike DPMS, it
 * doesn't disturb capture and isn't woken by injected mouse/keyboard. Per platform:
 *   - X11:      xrandr --brightness 0 (a CRTC gamma ramp)
 *   - Windows:  SetDeviceGammaRamp to an all-zero ramp
 *   - macOS:    CGSetDisplayTransferByFormula with max=0 (output pinned black)
 *   - Wayland:  wlr-gamma-control (wlroots compositors: sway/Hyprland/wayfire)
 * Best-effort and idempotent; a no-op where unavailable (e.g. Android, or GNOME/KDE Wayland).
 */
#ifndef BSDR_SCREENBLANK_H
#define BSDR_SCREENBLANK_H

/* on != 0 -> blank the physical display(s); on == 0 -> restore. Safe to call repeatedly. */
void bsdr_screen_blank(int on);

/* Recover a monitor left blanked by a PREVIOUS run that died without restoring (crash / SIGKILL /
 * power loss). Call once at startup. Only the PERSISTENT backends need it — X11 xrandr and the
 * Windows/macOS gamma ramps outlive the process; the Wayland wlr-gamma-control path auto-restores when
 * the client disconnects, so it is deliberately skipped here (touching it would only override the
 * compositor's night-light for our whole run). No-op where unavailable. */
void bsdr_screen_blank_reset(void);

#endif /* BSDR_SCREENBLANK_H */
