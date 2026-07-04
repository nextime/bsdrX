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
/* Computer-control executor: dispatch + the open_app safety guard.
 * (Only exercises open_app, which doesn't touch the injector — safe to run.) */
#include "bsdr/compcontrol.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    int fail = 0;
    bsdr_compcontrol *cc = bsdr_compcontrol_new(NULL);  /* open_app ignores inj */
    char res[64];

    bsdr_compcontrol_exec("open_app", "{\"name\":\"true\"}", res, sizeof(res), cc);
    if (strcmp(res, "launched") == 0) printf("PASS open_app_launch\n");
    else { printf("FAIL open_app_launch (%s)\n", res); fail++; }

    bsdr_compcontrol_exec("open_app", "{\"name\":\"x; rm -rf /\"}", res, sizeof(res), cc);
    if (strcmp(res, "rejected") == 0) printf("PASS open_app_reject_metachars\n");
    else { printf("FAIL open_app_reject (%s)\n", res); fail++; }

    bsdr_compcontrol_exec("open_app", "{\"name\":\"a && b\"}", res, sizeof(res), cc);
    if (strcmp(res, "rejected") == 0) printf("PASS open_app_reject_chain\n");
    else { printf("FAIL open_app_reject_chain (%s)\n", res); fail++; }

    bsdr_compcontrol_exec("bogus_tool", "{}", res, sizeof(res), cc);
    if (strcmp(res, "unknown tool") == 0) printf("PASS unknown_tool\n");
    else { printf("FAIL unknown_tool (%s)\n", res); fail++; }

    bsdr_compcontrol_free(cc);
    printf(fail ? "\nFAILED (%d)\n" : "\nOK - computer-control executor passed\n", fail);
    return fail ? 1 : 0;
}
