/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
#include "bsdr/updatecheck.h"
#include "bsdr/app.h"
#include "bsdr/version.h"
#include "bsdr/httpc.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Compare two dotted numeric version strings component by component. Missing components read as 0,
 * so "0.1" == "0.1.0". Non-numeric junk stops that component. */
int bsdr_version_cmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a || *b) {
        long na = strtol(a, NULL, 10);
        long nb = strtol(b, NULL, 10);
        if (na != nb) return na < nb ? -1 : 1;
        /* advance each past this numeric component and the following dot */
        while (*a && *a != '.') a++;
        while (*b && *b != '.') b++;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
    }
    return 0;
}

static void trim(char *s) {
    /* strip surrounding whitespace / newlines and anything after the first whitespace */
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (char *q = s; *q; q++) {
        if (isspace((unsigned char)*q)) { *q = '\0'; break; }
    }
}

/* One check: fetch the remote version and flag an update if it is strictly newer. Quiet on failure. */
static void check_once(bsdr_app *a) {
    char resp[8192];
    int r = bsdr_http_request("GET", BSDR_UPDATE_URL, NULL, 0, NULL, NULL, 0, resp, sizeof resp);
    if (r < 0 || bsdr_http_status(resp) / 100 != 2) return;
    const char *body = bsdr_http_body(resp);
    if (!body) return;
    char latest[32] = "";
    snprintf(latest, sizeof latest, "%.31s", body);
    trim(latest);
    if (!latest[0] || (latest[0] != '.' && !isdigit((unsigned char)latest[0]))) return;  /* not a version */

    if (bsdr_version_cmp(latest, BSDR_VERSION) > 0) {
        bsdr_mutex_lock(a->lock);
        a->update_available = true;
        snprintf(a->update_latest, sizeof a->update_latest, "%s", latest);
        bsdr_mutex_unlock(a->lock);
        BSDR_INFO("bsdr.update", "a newer release is available: %s (running %s)", latest, BSDR_VERSION);
    }
    /* remote older or equal => leave the flag as-is (no-op) */
}

static void checker_fn(void *arg) {
    bsdr_app *a = (bsdr_app *)arg;
    check_once(a);                                  /* immediate first check */
    while (!a->update_stop) {
        for (int i = 0; i < 3600 && !a->update_stop; i++) bsdr_sleep_ms(1000);  /* ~1h, wake to exit */
        if (a->update_stop) break;
        check_once(a);
    }
}

void bsdr_updatecheck_start(bsdr_app *a) {
    if (!a) return;
    bsdr_thread_start_detached(checker_fn, a);
}
