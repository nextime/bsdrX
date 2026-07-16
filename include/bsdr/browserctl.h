/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Minimal Chrome DevTools Protocol (CDP) client for the owner-only "browser control" tool.
 *
 * Point it at a browser started with --remote-debugging-port=9222; it discovers the active page's
 * debugger WebSocket via GET <endpoint>/json, connects (plain ws://, localhost debug endpoint) and
 * issues a single CDP command. Two convenience calls cover the tool set: navigate to a URL, and
 * evaluate JavaScript in the page (which also covers clicking, reading text, filling forms, etc.).
 * Owner-only + disabled by default + gated on a configured endpoint (see BSDR_TG_BROWSER). */
#ifndef BSDR_BROWSERCTL_H
#define BSDR_BROWSERCTL_H

#include <stddef.h>

/* Navigate the active page to `url`. Writes a short status into `result`. Returns 0 on success. */
int bsdr_browser_navigate(const char *http_endpoint, const char *url, char *result, size_t cap);

/* Evaluate a JavaScript expression in the active page and return its (stringified) value into
 * `result`. Use it to click ("document.querySelector('a').click()"), read
 * ("document.title" / "document.body.innerText.slice(0,2000)"), fill forms, etc. Returns 0 on ok. */
int bsdr_browser_eval(const char *http_endpoint, const char *expr, char *result, size_t cap);

#endif /* BSDR_BROWSERCTL_H */
