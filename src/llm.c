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
/* OpenAI-compatible chat-completions client with tool calling. */
#include "bsdr/llm.h"
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* computer-control tools advertised to the model */
static const char TOOLS[] =
"["
"{\"type\":\"function\",\"function\":{\"name\":\"type_text\",\"description\":"
"\"Type text at the current focus\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"key\",\"description\":"
"\"Press a key combo, e.g. ctrl+c, alt+Tab, Return\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"keys\":{\"type\":\"string\"}},\"required\":[\"keys\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"click\",\"description\":"
"\"Click at a normalized screen position (0..1)\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
"\"button\":{\"type\":\"string\"}},\"required\":[\"x\",\"y\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"scroll\",\"description\":"
"\"Scroll vertically (positive=up)\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"amount\":{\"type\":\"number\"}},\"required\":[\"amount\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"open_app\",\"description\":"
"\"Launch an application by command name\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}}"
"]";

/* Extra tool offered only when a screenshot provider is available. The model calls
 * it when it needs to SEE the desktop (find UI, read content, decide where to click);
 * the captured image is then attached as an image message for the next turn. */
static const char SHOT_TOOL[] =
",{\"type\":\"function\",\"function\":{\"name\":\"take_screenshot\",\"description\":"
"\"Capture the current desktop and attach it as an image so you can see what is on "
"screen. Call this only when the task requires looking at the screen (locating UI "
"elements, reading content, or deciding where to click).\","
"\"parameters\":{\"type\":\"object\",\"properties\":{}}}}";

/* extract a brace-matched object/array starting at the '{' or '[' after `key`. */
static bool extract_json_value(const char *s, const char *key, char *out, size_t cap) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\n') p++;
    if (*p != '{' && *p != '[') return false;
    char open = *p, close = open == '{' ? '}' : ']';
    int depth = 0; bool instr = false; size_t o = 0;
    for (; *p; p++) {
        if (instr) { if (*p == '\\') { if (o+2<cap){out[o++]=*p;out[o++]=p[1];} p++; continue; }
                     if (*p == '"') instr = false; }
        else if (*p == '"') instr = true;
        else if (*p == open) depth++;
        else if (*p == close) depth--;
        if (o + 1 < cap) out[o++] = *p;
        if (!instr && depth == 0) { out[o] = '\0'; return true; }
    }
    return false;
}

/* base64 (standard alphabet, no line breaks). Returns bytes written (excl. NUL). */
static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        if (o + 4 >= cap) break;
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < n) v |= in[i+2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

#define SHOT_JPEG_CAP (1024 * 1024)   /* max screenshot the model can pull per call */

/* Capture via `img_cb`, base64 it, and append a user image message the model sees
 * next turn. Returns bytes appended to msgs (0 on failure). */
static int append_screenshot(char *msgs, int mn, size_t msgs_cap,
                             bsdr_llm_image_cb img_cb, void *img_user) {
    uint8_t *jpeg = malloc(SHOT_JPEG_CAP);
    if (!jpeg) return 0;
    int jn = img_cb(img_user, jpeg, SHOT_JPEG_CAP);
    int added = 0;
    if (jn > 0) {
        size_t need = ((size_t)jn / 3) * 4 + 4096;
        if (mn > 0 && (size_t)mn + need < msgs_cap) {
            char *b64 = malloc(need);
            if (b64) {
                b64_encode(jpeg, (size_t)jn, b64, need);
                added = snprintf(msgs + mn, msgs_cap - mn,
                    ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
                    "\"Here is the current screen.\"},{\"type\":\"image_url\","
                    "\"image_url\":{\"url\":\"data:image/jpeg;base64,%s\",\"detail\":\"auto\"}}]}",
                    b64);
                free(b64);
            }
        } else BSDR_WARN("bsdr.llm", "screenshot too big for context, skipped");
    }
    free(jpeg);
    return added > 0 ? added : 0;
}

bool bsdr_llm_run_ex(const bsdr_llm_config *cfg, const char *system_prompt,
                     const char *user_text, bsdr_llm_tool_cb cb, void *user,
                     bsdr_llm_image_cb img_cb, void *img_user,
                     const volatile int *abort, char *out, size_t out_len) {
    if (!cfg->endpoint[0]) { BSDR_WARN("bsdr.llm", "no LLM endpoint configured"); return false; }

    /* Room for a couple of on-demand screenshots (base64) when vision is offered. */
    size_t msgs_cap = 96 * 1024 + (img_cb ? 3 * 1024 * 1024 : 0);
    size_t body_cap = msgs_cap + 8192;
    size_t resp_cap = 128 * 1024;
    char *msgs = malloc(msgs_cap), *body = malloc(body_cap), *resp = malloc(resp_cap);
    char *tools = NULL;
    bool ok = false;
    if (!msgs || !body || !resp) { BSDR_ERROR("bsdr.llm", "oom"); goto done; }

    /* tools = base set, plus take_screenshot when a provider is available */
    if (img_cb) {
        tools = malloc(sizeof(TOOLS) + sizeof(SHOT_TOOL));
        if (tools)   /* TOOLS without its trailing ']' + the shot tool + ']' */
            snprintf(tools, sizeof(TOOLS) + sizeof(SHOT_TOOL), "%.*s%s]",
                     (int)strlen(TOOLS) - 1, TOOLS, SHOT_TOOL);
    }
    const char *TOOLSET = tools ? tools : TOOLS;

    char sysE[2048], usrE[4096];
    bsdr_json_escape(sysE, sizeof(sysE), system_prompt && *system_prompt ? system_prompt :
        "You control a Linux desktop streamed into VR. Use the tools to fulfil the "
        "user's spoken request; reply briefly.");
    bsdr_json_escape(usrE, sizeof(usrE), user_text);
    int mn = snprintf(msgs, msgs_cap,
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}", sysE, usrE);

    char auth[320]; bsdr_http_header h[1]; int nh = 0;
    if (cfg->token[0]) { snprintf(auth, sizeof(auth), "Bearer %s", cfg->token);
                         h[0].name = "Authorization"; h[0].value = auth; nh = 1; }

    for (int round = 0; round < 5; round++) {
        if (abort && *abort) { snprintf(out, out_len, "stopped"); ok = true; goto done; }
        int bn = snprintf(body, body_cap,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"tool_choice\":\"auto\"}",
            cfg->model[0] ? cfg->model : "gpt-4o-mini", msgs, TOOLSET);

        int r = bsdr_http_request("POST", cfg->endpoint, h, nh,
                                  "application/json", body, (size_t)bn, resp, resp_cap);
        if (r < 0) { BSDR_ERROR("bsdr.llm", "request failed"); goto done; }
        if (bsdr_http_status(resp) / 100 != 2) {
            BSDR_ERROR("bsdr.llm", "HTTP %d", bsdr_http_status(resp)); goto done;
        }
        const char *bdy = bsdr_http_body(resp);
        if (!bdy) goto done;

        static char message[16 * 1024];
        if (!extract_json_value(bdy, "message", message, sizeof(message))) goto done;

        if (strstr(message, "\"tool_calls\"")) {
            /* append the assistant message, then a tool result per call */
            mn += snprintf(msgs + mn, msgs_cap - mn, ",%s", message);
            int want_shot = 0;
            const char *p = strstr(message, "tool_calls");
            while ((p = strstr(p, "\"id\"")) != NULL) {
                char id[96] = "", name[64] = "", args[2048] = "", result[512] = "no-op";
                bsdr_json_get_str(p, "id", id, sizeof(id));
                const char *fp = strstr(p, "\"function\"");
                if (fp) {
                    bsdr_json_get_str(fp, "name", name, sizeof(name));
                    bsdr_json_get_str(fp, "arguments", args, sizeof(args));
                }
                if (abort && *abort) snprintf(result, sizeof(result), "aborted by user");
                else if (img_cb && !strcmp(name, "take_screenshot")) {
                    want_shot = 1;
                    snprintf(result, sizeof(result), "screenshot captured; see the image below");
                } else if (name[0] && cb) {
                    cb(name, args, result, sizeof(result), user);
                }
                BSDR_INFO("bsdr.llm", "tool %s(%s) -> %s", name, args, result);
                char resE[600]; bsdr_json_escape(resE, sizeof(resE), result);
                mn += snprintf(msgs + mn, msgs_cap - mn,
                    ",{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}", id, resE);
                p = fp ? fp + 10 : p + 4;
            }
            if (want_shot && img_cb)     /* attach the captured screen for the next turn */
                mn += append_screenshot(msgs, mn, msgs_cap, img_cb, img_user);
            continue;   /* let the model see the results */
        }
        /* final answer */
        if (bsdr_json_get_str(message, "content", out, out_len) && out[0]) {
            BSDR_INFO("bsdr.llm", "reply: %.80s", out);
        } else {
            snprintf(out, out_len, "done");
        }
        ok = true; goto done;
    }
    snprintf(out, out_len, "(stopped after tool rounds)");
    ok = true;
done:
    free(msgs); free(body); free(resp); free(tools);
    return ok;
}

bool bsdr_llm_run(const bsdr_llm_config *cfg, const char *system_prompt,
                  const char *user_text, bsdr_llm_tool_cb cb, void *user,
                  char *out, size_t out_len) {
    return bsdr_llm_run_ex(cfg, system_prompt, user_text, cb, user, NULL, NULL, NULL, out, out_len);
}
