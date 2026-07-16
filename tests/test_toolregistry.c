/*
 * bsdrX — tool registry unit test (ABI 4 bot host-service surface).
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>. GPLv3-or-later.
 *
 * Exercises registration/replace, owner-scoped + by-name unregister, permission-group filtering in
 * both list_json and invoke, and handler dispatch (args + caller level + result).
 */
#include "bsdr/toolregistry.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* two arbitrary permission-group bits (stand-ins for BSDR_TG_*) */
#define GROUP_A (1u << 1)
#define GROUP_B (1u << 3)

/* owners (any distinct pointers) */
static int owner_a, owner_b;

static int last_level = -1;
static char last_args[128];

/* echoes the args + records the caller level; returns a fixed JSON result */
static int tool_echo(void *state, const char *args_json, int caller_level, char *out, size_t cap) {
    (void)state;
    last_level = caller_level;
    snprintf(last_args, sizeof last_args, "%s", args_json ? args_json : "");
    snprintf(out, cap, "{\"ok\":true}");
    return 1;
}

int main(void) {
    /* register two tools under different groups + owners */
    CHECK(bsdr_tools_register("alpha", "A tool", "{\"type\":\"object\"}", GROUP_A, tool_echo, &owner_a), "register alpha");
    CHECK(bsdr_tools_register("beta",  "B tool", "", GROUP_B, tool_echo, &owner_b), "register beta");
    CHECK(bsdr_tools_count() == 2, "count == 2 after two registers");

    /* re-register 'alpha' replaces in place (still 2 tools) */
    CHECK(bsdr_tools_register("alpha", "A tool v2", "{}", GROUP_A, tool_echo, &owner_a), "re-register alpha");
    CHECK(bsdr_tools_count() == 2, "count still 2 after replace");

    /* list filtered by allowed_mask: only tools whose group is within the mask appear */
    char buf[2048];
    bsdr_tools_list_json(GROUP_A, buf, sizeof buf);
    CHECK(strstr(buf, "\"alpha\"") != NULL, "alpha listed for GROUP_A mask");
    CHECK(strstr(buf, "\"beta\"")  == NULL, "beta NOT listed for GROUP_A mask");
    /* empty schema is emitted as {} so 'parameters' is always valid JSON */
    bsdr_tools_list_json(GROUP_A | GROUP_B, buf, sizeof buf);
    CHECK(strstr(buf, "\"beta\"") != NULL, "beta listed when its group is in the mask");
    CHECK(strstr(buf, "\"parameters\":{}") != NULL, "empty schema -> parameters:{}");

    /* invoke honours the caller mask: permitted */
    char out[128];
    int ok = bsdr_tools_invoke("alpha", "{\"x\":1}", 3, GROUP_A | GROUP_B, out, sizeof out);
    CHECK(ok == 1, "invoke alpha permitted");
    CHECK(last_level == 3, "caller level passed to handler");
    CHECK(strcmp(last_args, "{\"x\":1}") == 0, "args passed to handler");
    CHECK(strstr(out, "\"ok\":true") != NULL, "handler result returned");

    /* invoke denied when the caller mask lacks the tool's group (defence in depth) */
    last_level = -1;
    ok = bsdr_tools_invoke("beta", "{}", 3, GROUP_A, out, sizeof out);
    CHECK(ok == 0, "invoke beta denied when mask lacks GROUP_B");
    CHECK(last_level == -1, "denied tool handler not called");

    /* unknown tool -> 0 */
    CHECK(bsdr_tools_invoke("nope", "{}", 3, ~0u, out, sizeof out) == 0, "unknown tool -> 0");

    /* by-name unregister */
    bsdr_tools_unregister("beta");
    CHECK(bsdr_tools_count() == 1, "count 1 after unregister beta");
    CHECK(bsdr_tools_invoke("beta", "{}", 3, ~0u, out, sizeof out) == 0, "beta gone after unregister");

    /* owner-scoped unregister drops everything for that owner */
    CHECK(bsdr_tools_register("gamma", "C", "{}", GROUP_A, tool_echo, &owner_a), "register gamma under owner_a");
    CHECK(bsdr_tools_count() == 2, "count 2 (alpha+gamma, both owner_a)");
    bsdr_tools_unregister_owner(&owner_a);
    CHECK(bsdr_tools_count() == 0, "owner unregister drops all owner_a tools");

    if (failures == 0) printf("PASS: test_toolregistry\n");
    else fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
