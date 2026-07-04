/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* One-shot desktop screenshot as a JPEG, for the vision LLM. Grabs the X11 root
 * window, downscales so the larger side is <= max_dim, and MJPEG-encodes it.
 * Returns the JPEG byte count written into `out` (0 on failure / unsupported
 * build). Only the desktop capture (Linux + video) build can produce one. */
#ifndef BSDR_SCREENSHOT_H
#define BSDR_SCREENSHOT_H

#include <stddef.h>
#include <stdint.h>

int bsdr_screenshot_jpeg(int max_dim, uint8_t *out, size_t cap);

#endif /* BSDR_SCREENSHOT_H */
