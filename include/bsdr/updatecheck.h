/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Background release-update check: every hour, fetch the latest published version string and, if it
 * is strictly newer than this build, flag it so the web UI can suggest upgrading. Entirely
 * best-effort — every failure is a quiet no-op. */
#ifndef BSDR_UPDATECHECK_H
#define BSDR_UPDATECHECK_H

typedef struct bsdr_app bsdr_app;

/* URL served with only the latest release's version number (e.g. "0.0.4\n"). */
#define BSDR_UPDATE_URL  "https://bigscreen.nexlab.net/bsdrxversion.txt"
/* Where the web UI banner points ("get the new version"), opened in a new tab. */
#define BSDR_UPDATE_HOME "https://bigscreen.nexlab.net"

/* Start the detached hourly checker (does one check immediately). Safe no-op if already running. */
void bsdr_updatecheck_start(bsdr_app *a);

/* Compare dotted numeric versions. >0 if a>b, 0 if equal, <0 if a<b. Exposed for testing. */
int  bsdr_version_cmp(const char *a, const char *b);

#endif /* BSDR_UPDATECHECK_H */
