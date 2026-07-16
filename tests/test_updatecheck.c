/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Dotted version comparison: the update banner must fire ONLY when the remote is strictly newer. */
#include "bsdr/updatecheck.h"
#include <stdio.h>

static int fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fail++; } } while (0)

int main(void) {
    CHECK(bsdr_version_cmp("0.0.3", "0.0.2") > 0, "newer_patch");
    CHECK(bsdr_version_cmp("0.1.0", "0.0.9") > 0, "newer_minor_beats_patch");
    CHECK(bsdr_version_cmp("1.0.0", "0.9.9") > 0, "newer_major");
    CHECK(bsdr_version_cmp("0.0.2", "0.0.3") < 0, "older_patch");
    CHECK(bsdr_version_cmp("0.0.3", "0.0.3") == 0, "equal");
    CHECK(bsdr_version_cmp("0.1",   "0.1.0") == 0, "missing_component_is_zero");
    CHECK(bsdr_version_cmp("0.0.10", "0.0.9") > 0, "numeric_not_lexical");
    CHECK(bsdr_version_cmp("0.0.3", "0.0.30") < 0, "numeric_not_lexical_2");

    /* the guard the checker uses: only remote strictly-greater => notify */
    CHECK(!(bsdr_version_cmp("0.0.2", "0.0.3") > 0), "no_downgrade_notify");

    printf(fail ? "\nFAILED (%d)\n" : "\nOK - updatecheck passed\n", fail);
    return fail ? 1 : 0;
}
