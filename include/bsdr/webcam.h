/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* webcam.h — enumerate the camera devices available to the platform video backend.
 *
 * The returned `id` is exactly what bsdr_capture_config.webcam expects for this platform:
 *   - Linux:   the "/dev/videoN" path (v4l2)
 *   - Windows: the DirectShow friendly name (opened as "video=<name>")
 *   - macOS:   the avfoundation device index as a string ("0", "1", …)
 * `name` is a human label for the web-UI dropdown. Android enumerates its cameras in Kotlin
 * (CameraManager), not here.
 */
#ifndef BSDR_WEBCAM_H
#define BSDR_WEBCAM_H

typedef struct {
    char id[256];    /* platform device spec for cfg.webcam */
    char name[128];  /* friendly label for the UI */
} bsdr_webcam_dev;

/* Fill up to `max` devices into `out`; returns the count found (>=0), or 0 if none/unsupported. */
int bsdr_webcam_list(bsdr_webcam_dev *out, int max);

#endif /* BSDR_WEBCAM_H */
