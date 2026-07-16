/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* In-app Plugin Store client.
 *
 * Talks to a bsdrx-plugstore server (default https://bigscreen.nexlab.net/bsdrxstore) so the operator
 * can, from the bsdrX web panel: create/login to a store account (persisted), browse the catalog,
 * buy paid plugins, download/update plugins for this platform+ABI, and enable/disable them locally.
 *
 * All calls are server-to-server from the agent's C backend (the store sets no CORS headers and its
 * register/login are not browser-cross-origin friendly), so the web UI drives this via the
 * /api/plugstore/ handlers in webui.c. Downloaded plugins are installed flat into <config_dir>/plugins and picked up by
 * the loader (src/plugin.c) on reload — no rebuild needed. The persisted credential is a license key
 * (prefix bslk_); the password is never stored. */
#ifndef BSDR_PLUGSTORE_H
#define BSDR_PLUGSTORE_H

#include <stddef.h>

/* Default store base URL (overridable from the UI; persisted in <config_dir>/plugstore.conf). */
#define BSDR_PLUGSTORE_DEFAULT_URL "https://bigscreen.nexlab.net/bsdrxstore"

/* ---- account + configuration ---------------------------------------------------------------- */

/* Write a status JSON describing the store connection and locally-installed plugins:
 *   {"ok":true,"url":..,"loggedIn":bool,"email":..,"platform":..,"arch":..,
 *    "installed":[{"name":..,"enabled":bool,"loaded":bool},..]}
 * Returns the number of bytes written. */
size_t bsdr_plugstore_status_json(char *out, size_t cap);

/* Persist a new store base URL (trailing slash trimmed). Returns 1 on success. */
int  bsdr_plugstore_set_url(const char *url);

/* Create an account / log in; on success persist the returned license key. Return 1 on success; on
 * failure return 0 and copy a short reason into err (if non-NULL). Blocking HTTPS. */
int  bsdr_plugstore_register(const char *email, const char *pw, char *err, size_t errcap);
int  bsdr_plugstore_login(const char *email, const char *pw, char *err, size_t errcap);
/* Sign in by pasting a license key minted on the store's /account page (verified via GET /api/v1/me).
 * Works for EVERY account — password or OAuth, admin or not — since it needs no password. Same return
 * contract as above. */
int  bsdr_plugstore_login_key(const char *key, char *err, size_t errcap);

/* Forget the stored license key (local only; the key remains valid until revoked in the store). */
void bsdr_plugstore_logout(void);

/* ---- catalog / purchase / install ----------------------------------------------------------- */

/* Fetch the store catalog (relayed verbatim as {"plugins":[..]}). Sends the license key if logged in
 * so private/entitled plugins appear. Returns 1 and writes JSON into out; on failure returns 0 and a
 * reason into err. */
int  bsdr_plugstore_catalog_json(char *out, size_t cap, char *err, size_t errcap);

/* Compose the browser purchase URL (<base>/buy/<slug>) for a paid plugin. Returns 1 on success. */
int  bsdr_plugstore_buy_url(const char *slug, char *out, size_t cap);

/* Download the newest build of <slug> compatible with this platform+arch+ABI, install it into
 * <config_dir>/plugins, and reload plugins so it takes effect. Returns 1 on success; on failure 0 and
 * a reason into err (e.g. not entitled -> buy first, or no build for this platform). Blocking. */
int  bsdr_plugstore_download(const char *slug, char *err, size_t errcap);

/* ---- local plugin management ---------------------------------------------------------------- */

/* Enable (on!=0) or disable a locally-installed plugin by descriptor name, then reload. A disabled
 * plugin stays on disk but is skipped by the loader. Returns 1 on success. */
int  bsdr_plugstore_set_enabled(const char *name, int on, char *err, size_t errcap);

/* Delete a locally-installed plugin's shared object (from <config_dir>/plugins) and reload. Only
 * store-installed plugins (in the per-user dir) can be removed this way. Returns 1 on success. */
int  bsdr_plugstore_remove(const char *name, char *err, size_t errcap);

/* Used by the plugin loader (src/plugin.c) to skip a plugin the operator disabled. Returns 1 if the
 * descriptor name is in <config_dir>/plugins.disabled. */
int  bsdr_plugin_name_disabled(const char *name);

#endif /* BSDR_PLUGSTORE_H */
