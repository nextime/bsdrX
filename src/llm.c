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
#include "bsdr/llmctx.h"
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/acl.h"    /* BSDR_TG_* tool-group bits */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tool catalog: each tool tagged with the access group that may use it. The advertised tool list is
 * built per request from the caller's toolmask, so a speaker only ever sees the tools their level
 * allows. Names/groups here must match the executor's dispatch + guard (compcontrol.c). */
typedef struct { const char *name; uint32_t group; const char *json; } bsdr_tool_def;
static const bsdr_tool_def TOOL_DEFS[] = {
  /* --- computer control (owner) --- */
  { "type_text", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"type_text\",\"description\":"
    "\"Type text at the current focus\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}}" },
  { "key", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"key\",\"description\":"
    "\"Press a key combo, e.g. ctrl+c, alt+Tab, Return\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"keys\":{\"type\":\"string\"}},\"required\":[\"keys\"]}}}" },
  { "click", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"click\",\"description\":"
    "\"Click at a normalized screen position (0..1)\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
    "\"button\":{\"type\":\"string\"}},\"required\":[\"x\",\"y\"]}}}" },
  { "scroll", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"scroll\",\"description\":"
    "\"Scroll vertically (positive=up)\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"amount\":{\"type\":\"number\"}},\"required\":[\"amount\"]}}}" },
  { "open_app", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"open_app\",\"description\":"
    "\"Launch an application by command name\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}}" },
  { "shell_exec", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"shell_exec\",\"description\":"
    "\"Run a shell command on the operator's computer and return its output (owner only). Use for "
    "tasks the desktop GUI tools can't do.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}}" },
  { "read_file", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":"
    "\"Read a text file from the operator's computer and return its contents (owner only).\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}}" },
  { "write_file", BSDR_TG_COMPUTER,
    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":"
    "\"Write text to a file on the operator's computer, overwriting it (owner only).\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}}" },
  /* --- public (friend and up) --- */
  { "speak", BSDR_TG_PUBLIC,
    "{\"type\":\"function\",\"function\":{\"name\":\"speak\",\"description\":"
    "\"Say something out loud via text-to-speech (spoken into the VR room, or the desktop audio, per "
    "the operator's setting). Use for a short spoken reply or confirmation. Only speaks if TTS is "
    "enabled.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}}" },
  { "web_search", BSDR_TG_PUBLIC,
    "{\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"description\":"
    "\"Search the web for a query and return result snippets. Use for current facts you don't know.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}}}" },
  { "web_read", BSDR_TG_PUBLIC,
    "{\"type\":\"function\",\"function\":{\"name\":\"web_read\",\"description\":"
    "\"Fetch a web page by URL and return its readable text (tags stripped).\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}}}" },
  { "stop_talking", BSDR_TG_PUBLIC,
    "{\"type\":\"function\",\"function\":{\"name\":\"stop_talking\",\"description\":"
    "\"Stop the current spoken reply / cancel what you are doing. Use when asked to be quiet or "
    "stop.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}" },
  /* --- moderation / host tools (host + owner) --- */
  { "kick_user", BSDR_TG_MODERATOR,
    "{\"type\":\"function\",\"function\":{\"name\":\"kick_user\",\"description\":"
    "\"Remove a person from the room by their display name (they can rejoin).\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"username\":{\"type\":\"string\"}},\"required\":[\"username\"]}}}" },
  { "ban_user", BSDR_TG_MODERATOR,
    "{\"type\":\"function\",\"function\":{\"name\":\"ban_user\",\"description\":"
    "\"Remove a person from the room and keep them out — they are auto-removed if they rejoin while "
    "the bot is present.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"username\":{\"type\":\"string\"}},\"required\":[\"username\"]}}}" },
  { "mic_check", BSDR_TG_MODERATOR,
    "{\"type\":\"function\",\"function\":{\"name\":\"mic_check\",\"description\":"
    "\"Age-verify a person: the bot asks them to speak, listens, and if they seem under 18 (or don't "
    "respond) removes and bans them. Use on an unknown/suspicious participant.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"username\":{\"type\":\"string\"}},\"required\":[\"username\"]}}}" },
  { "mic_check_enable", BSDR_TG_MODERATOR,
    "{\"type\":\"function\",\"function\":{\"name\":\"mic_check_enable\",\"description\":"
    "\"Turn automatic mic-check on/off: when on, unknown people who join are age-verified "
    "automatically.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":[\"on\"]}}}" },
  { "translate_next", BSDR_TG_MODERATOR,
    "{\"type\":\"function\",\"function\":{\"name\":\"translate_next\",\"description\":"
    "\"Translate the NEXT thing a person in the room says into a target language and speak the "
    "translation aloud. Use when asked to translate what someone (or the requester themselves) is "
    "about to say. To translate a phrase given right now, just reply with the translation instead. "
    "username = whose next utterance to translate (the requester's own name to translate their own "
    "speech); to_language = the language to translate into.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"username\":{\"type\":\"string\"},\"to_language\":{\"type\":\"string\"}},"
    "\"required\":[\"username\",\"to_language\"]}}}" },
  /* --- bot control (host in own room / owner) --- */
  { "follow_me", BSDR_TG_BOTCTL,
    "{\"type\":\"function\",\"function\":{\"name\":\"follow_me\",\"description\":"
    "\"Toggle follow-me: the bot re-joins whatever room the owner moves into.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":[\"on\"]}}}" },
  { "leave_room", BSDR_TG_BOTCTL,
    "{\"type\":\"function\",\"function\":{\"name\":\"leave_room\",\"description\":"
    "\"Make the bot leave the current room.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}" },
  { "restart_bot", BSDR_TG_BOTCTL,
    "{\"type\":\"function\",\"function\":{\"name\":\"restart_bot\",\"description\":"
    "\"Make the bot leave and re-join the room.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}" },
  { "stay_with", BSDR_TG_BOTCTL,
    "{\"type\":\"function\",\"function\":{\"name\":\"stay_with\",\"description\":"
    "\"Owner only: have the bot stay in and moderate the named user's room even after the owner "
    "leaves, until told to leave.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"username\":{\"type\":\"string\"}},\"required\":[\"username\"]}}}" },
  /* --- admin (owner) --- */
  { "authorize", BSDR_TG_ADMIN,
    "{\"type\":\"function\",\"function\":{\"name\":\"authorize\",\"description\":"
    "\"Owner only: grant access. what='friend' adds the named user as a friend.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"what\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"}},"
    "\"required\":[\"what\",\"username\"]}}}" },
  { "deauthorize", BSDR_TG_ADMIN,
    "{\"type\":\"function\",\"function\":{\"name\":\"deauthorize\",\"description\":"
    "\"Owner only: revoke access. what='friend' removes the named user from friends.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"what\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"}},"
    "\"required\":[\"what\",\"username\"]}}}" },
  { "set_access", BSDR_TG_ADMIN,
    "{\"type\":\"function\",\"function\":{\"name\":\"set_access\",\"description\":"
    "\"Owner only: toggle an access setting. which='friends'|'hosts' (master access) or a tool group "
    "'public'|'moderator'|'botctl'|'computer'|'admin'; enabled=true/false.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"which\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"which\",\"enabled\"]}}}" },
  { "reset_room", BSDR_TG_ADMIN,
    "{\"type\":\"function\",\"function\":{\"name\":\"reset_room\",\"description\":"
    "\"Owner only: reset the room by removing every participant (the official kick) so everyone drops "
    "to the lobby and can rejoin a fresh room. Use to recover a stuck/frozen room.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}}" },
  /* --- browser control via CDP (owner only, opt-in; advertised only when the caller ORs
   *     BSDR_TG_BROWSER into the mask, i.e. enabled + a CDP endpoint configured) --- */
  { "browser_navigate", BSDR_TG_BROWSER,
    "{\"type\":\"function\",\"function\":{\"name\":\"browser_navigate\",\"description\":"
    "\"Open a URL in the operator's Chrome/Chromium (via the DevTools protocol).\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}}}" },
  { "browser_eval", BSDR_TG_BROWSER,
    "{\"type\":\"function\",\"function\":{\"name\":\"browser_eval\",\"description\":"
    "\"Run a JavaScript expression in the active browser tab and return its value. Use it to read the "
    "page (e.g. document.title, document.body.innerText.slice(0,2000)), click "
    "(document.querySelector('SEL').click()), or fill forms. Returns the evaluated value.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"expression\":{\"type\":\"string\"}},\"required\":[\"expression\"]}}}" },
};
#define N_TOOL_DEFS ((int)(sizeof TOOL_DEFS / sizeof TOOL_DEFS[0]))

/* Extra tool offered only when a screenshot provider is available AND the caller has the computer
 * group. The model calls it when it needs to SEE the desktop; the image is attached next turn. */
static const char SHOT_TOOL[] =
"{\"type\":\"function\",\"function\":{\"name\":\"take_screenshot\",\"description\":"
"\"Capture the current desktop and attach it as an image so you can see what is on "
"screen. Call this only when the task requires looking at the screen (locating UI "
"elements, reading content, or deciding where to click).\","
"\"parameters\":{\"type\":\"object\",\"properties\":{}}}}";

uint32_t bsdr_llm_tool_group(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < N_TOOL_DEFS; i++) if (strcmp(TOOL_DEFS[i].name, name) == 0) return TOOL_DEFS[i].group;
    if (strcmp(name, "take_screenshot") == 0) return BSDR_TG_COMPUTER;
    return 0;
}

/* Build the JSON tools array for `mask` (+ take_screenshot when vision is offered and allowed) into a
 * freshly malloc'd string. Returns NULL on OOM or if no tools are enabled. */
static char *build_tools(uint32_t mask, bool with_shot) {
    size_t cap = 64;
    for (int i = 0; i < N_TOOL_DEFS; i++) if (mask & TOOL_DEFS[i].group) cap += strlen(TOOL_DEFS[i].json) + 1;
    if (with_shot) cap += sizeof SHOT_TOOL + 1;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t o = 0; int n = 0;
    buf[o++] = '[';
    for (int i = 0; i < N_TOOL_DEFS; i++) {
        if (!(mask & TOOL_DEFS[i].group)) continue;
        if (n++) buf[o++] = ',';
        o += (size_t)snprintf(buf + o, cap - o, "%s", TOOL_DEFS[i].json);
    }
    if (with_shot) { if (n++) buf[o++] = ','; o += (size_t)snprintf(buf + o, cap - o, "%s", SHOT_TOOL); }
    buf[o++] = ']'; buf[o] = '\0';
    if (n == 0) { free(buf); return NULL; }
    return buf;
}

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

/* Capture via `img_cb`, base64 it, and push a user image message the model sees next turn.
 * Returns true if a message was added. */
static bool push_screenshot(bsdr_llmctx *ctx, bsdr_llm_image_cb img_cb, void *img_user) {
    uint8_t *jpeg = malloc(SHOT_JPEG_CAP);
    if (!jpeg) return false;
    int jn = img_cb(img_user, jpeg, SHOT_JPEG_CAP);
    bool ok = false;
    if (jn > 0) {
        size_t b64cap = ((size_t)jn / 3) * 4 + 8;
        size_t need = b64cap + 256;
        char *b64 = malloc(b64cap), *msg = malloc(need);
        if (b64 && msg) {
            b64_encode(jpeg, (size_t)jn, b64, b64cap);
            int n = snprintf(msg, need,
                "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
                "\"Here is the current screen.\"},{\"type\":\"image_url\","
                "\"image_url\":{\"url\":\"data:image/jpeg;base64,%s\",\"detail\":\"auto\"}}]}",
                b64);
            if (n > 0) ok = bsdr_llmctx_push_n(ctx, BSDR_MSG_USER, msg, (size_t)n);
        }
        free(b64); free(msg);
    }
    free(jpeg);
    return ok;
}

/* Read an integer JSON field (best-effort, top-level-ish); -1 if not found. */
static long json_int_field(const char *s, const char *key) {
    if (!s) return -1;
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    return strtol(p, NULL, 10);
}

/* Ask the model to summarize a rendered conversation slice into concise notes. `slice` is a
 * comma-joined list of message objects (no brackets). Writes the summary text into `out`. */
static bool summarize_slice(const bsdr_llm_config *cfg, bsdr_http_header *h, int nh,
                            const char *slice, char *resp, size_t resp_cap,
                            char *out, size_t out_cap) {
    size_t bc = strlen(slice) + 2048;
    char *body = malloc(bc);
    if (!body) return false;
    int bn = snprintf(body, bc,
        "{\"model\":\"%s\",\"messages\":[%s,{\"role\":\"user\",\"content\":\""
        "Summarize the conversation above into concise notes: keep decisions made, commands run and "
        "their key results, files created or edited, and what still needs doing. Be brief and "
        "factual.\"}]}",
        cfg->model[0] ? cfg->model : "gpt-4o-mini", slice);
    int r = (bn > 0) ? bsdr_http_request("POST", cfg->endpoint, h, nh, "application/json",
                                          body, (size_t)bn, resp, resp_cap) : -1;
    free(body);
    if (r < 0 || bsdr_http_status(resp) / 100 != 2) return false;
    const char *bdy = bsdr_http_body(resp);
    if (!bdy) return false;
    static char message[16 * 1024];
    if (!extract_json_value(bdy, "message", message, sizeof message)) return false;
    return bsdr_json_get_str(message, "content", out, out_cap) && out[0];
}

/* Compact `ctx` per `strategy`. For SUMMARY/HYBRID, first summarize the slice about to be dropped
 * (a network call) so the model keeps the gist. Never fatal — a failed summary degrades to a plain
 * truncate. `scratch`/`resp` are reused buffers from the caller. */
static void run_compaction(const bsdr_llm_config *cfg, bsdr_http_header *h, int nh, bsdr_llmctx *ctx,
                           int strategy, char *scratch, size_t scratch_cap, char *resp, size_t resp_cap) {
    int keep_tail = (strategy == BSDR_COMPACT_HYBRID) ? 12 : 6;
    char summary[2048] = "";
    const char *summ = NULL;
    if (strategy == BSDR_COMPACT_SUMMARY || strategy == BSDR_COMPACT_HYBRID) {
        int first, last;
        if (bsdr_llmctx_drop_range(ctx, keep_tail, &first, &last)) {
            size_t sl = bsdr_llmctx_render_range(ctx, first, last, scratch, scratch_cap);
            if (sl > 0 && summarize_slice(cfg, h, nh, scratch, resp, resp_cap, summary, sizeof summary))
                summ = summary;
        }
    }
    int before = bsdr_llmctx_count(ctx);
    int after  = bsdr_llmctx_compact(ctx, strategy, keep_tail, summ);
    if (after != before)
        BSDR_INFO("bsdr.llm", "compacted context %d -> %d messages (strategy %d%s)",
                  before, after, strategy, summ ? ", summarized" : "");
}

bool bsdr_llm_run_ex(const bsdr_llm_config *cfg, const char *system_prompt,
                     const char *user_text, bsdr_llm_tool_cb cb, void *user,
                     bsdr_llm_image_cb img_cb, void *img_user, uint32_t toolmask,
                     const volatile int *abort, char *out, size_t out_len) {
    if (!cfg->endpoint[0]) { BSDR_WARN("bsdr.llm", "no LLM endpoint configured"); return false; }

    /* Context management + agentic depth (0 => defaults). A coding task can run many rounds, so the
     * conversation is held in a bsdr_llmctx and COMPACTED once it passes the window threshold. */
    int  ctx_tokens  = cfg->context_tokens > 0 ? cfg->context_tokens : 32768;
    int  max_rounds  = cfg->max_rounds     > 0 ? cfg->max_rounds     : 24;
    bool compact_on  = cfg->compact_pct   >= 0;                     /* <0 disables compaction */
    int  compact_pct = cfg->compact_pct    > 0 ? cfg->compact_pct   : 80;
    int  strategy    = cfg->compact_strategy;
    long trigger_tokens = (long)ctx_tokens * compact_pct / 100;

    /* Render buffer must hold the whole (post-compaction) conversation, incl. any screenshot. */
    size_t msgs_cap = 768 * 1024 + (img_cb ? 4 * 1024 * 1024 : 0);
    size_t body_cap = msgs_cap + 16384;
    size_t resp_cap = 128 * 1024;
    char *msgs = malloc(msgs_cap), *body = malloc(body_cap), *resp = malloc(resp_cap);
    char *tools = NULL;
    bsdr_llmctx *ctx = bsdr_llmctx_new();
    bool ok = false;
    if (!msgs || !body || !resp || !ctx) { BSDR_ERROR("bsdr.llm", "oom"); goto done; }

    /* Tools advertised = those in the caller's mask; take_screenshot only with vision + computer. */
    bool with_shot = img_cb && (toolmask & BSDR_TG_COMPUTER);
    tools = build_tools(toolmask, with_shot);
    const char *TOOLSET = tools ? tools : "[]";

    char sysE[2048], usrE[4096];
    bsdr_json_escape(sysE, sizeof(sysE), system_prompt && *system_prompt ? system_prompt :
        "You control a Linux desktop streamed into VR. Use the tools to fulfil the "
        "user's spoken request; reply briefly.");
    bsdr_json_escape(usrE, sizeof(usrE), user_text);
    { char m[2400]; snprintf(m, sizeof m, "{\"role\":\"system\",\"content\":\"%s\"}", sysE);
      bsdr_llmctx_push(ctx, BSDR_MSG_SYSTEM, m); }
    { char m[4300]; snprintf(m, sizeof m, "{\"role\":\"user\",\"content\":\"%s\"}", usrE);
      bsdr_llmctx_push(ctx, BSDR_MSG_USER, m); }

    char auth[320]; bsdr_http_header h[1]; int nh = 0;
    if (cfg->token[0]) { snprintf(auth, sizeof(auth), "Bearer %s", cfg->token);
                         h[0].name = "Authorization"; h[0].value = auth; nh = 1; }

    long last_total_tokens = -1;   /* real usage from the server, if it reports it */
    for (int round = 0; round < max_rounds; round++) {
        if (abort && *abort) { snprintf(out, out_len, "stopped"); ok = true; goto done; }

        /* Compact BEFORE building the request if we're over the window threshold. Prefer the server's
         * real token count; fall back to a byte-based estimate. */
        if (compact_on) {
            long cur = last_total_tokens > 0 ? last_total_tokens : (long)bsdr_llmctx_est_tokens(ctx);
            if (cur > trigger_tokens && bsdr_llmctx_count(ctx) > 4) {
                run_compaction(cfg, h, nh, ctx, strategy, msgs, msgs_cap, resp, resp_cap);
                last_total_tokens = -1;   /* size unknown until the next response reports usage */
            }
        }

        size_t mlen = bsdr_llmctx_render(ctx, 0, msgs, msgs_cap);
        if (mlen == 0 && bsdr_llmctx_count(ctx) > 0) {
            BSDR_ERROR("bsdr.llm", "conversation too large to render"); goto done;
        }
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
        { long tt = json_int_field(bdy, "total_tokens"); if (tt > 0) last_total_tokens = tt; }

        static char message[16 * 1024];
        if (!extract_json_value(bdy, "message", message, sizeof(message))) goto done;

        if (strstr(message, "\"tool_calls\"")) {
            /* record the assistant message, then a tool result per call */
            bsdr_llmctx_push(ctx, BSDR_MSG_ASSISTANT, message);
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
                char tmsg[1200];
                snprintf(tmsg, sizeof tmsg,
                    "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}", id, resE);
                bsdr_llmctx_push(ctx, BSDR_MSG_TOOL, tmsg);
                p = fp ? fp + 10 : p + 4;
            }
            if (want_shot && img_cb)     /* attach the captured screen for the next turn */
                push_screenshot(ctx, img_cb, img_user);
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
    bsdr_llmctx_free(ctx);
    return ok;
}

bool bsdr_llm_run(const bsdr_llm_config *cfg, const char *system_prompt,
                  const char *user_text, bsdr_llm_tool_cb cb, void *user,
                  char *out, size_t out_len) {
    return bsdr_llm_run_ex(cfg, system_prompt, user_text, cb, user, NULL, NULL,
                           BSDR_TG_ALL, NULL, out, out_len);
}

/* Single model round-trip (loop-in-fullbot, PLAN-bot-plugin.md): POST one request built from a
 * caller-supplied messages array + tools array, and return the assistant `message` object JSON (content
 * + any tool_calls) in out. The AGENTIC LOOP — parsing tool_calls, invoking tools, appending results,
 * re-calling — belongs to the caller (fullbot); this only does the wire round-trip so model/prompt/loop
 * policy stays in the plugin, not the core. Returns 1 on success (out = the message object), else 0.
 * `messages_json` is a full JSON array string (e.g. [{"role":"system",…},{"role":"user",…}]);
 * `tools_json` a full array (NULL/"" => "[]"). */
int bsdr_llm_complete_once(const bsdr_llm_config *cfg, const char *messages_json,
                           const char *tools_json, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!cfg || !cfg->endpoint[0] || !messages_json || !out || cap == 0) return 0;
    const char *tools = (tools_json && tools_json[0]) ? tools_json : "[]";

    size_t body_cap = strlen(messages_json) + strlen(tools) + 4096;
    size_t resp_cap = 128 * 1024;
    char *body = malloc(body_cap), *resp = malloc(resp_cap);
    if (!body || !resp) { free(body); free(resp); BSDR_ERROR("bsdr.llm", "oom"); return 0; }

    char auth[320]; bsdr_http_header h[1]; int nh = 0;
    if (cfg->token[0]) { snprintf(auth, sizeof auth, "Bearer %s", cfg->token);
                         h[0].name = "Authorization"; h[0].value = auth; nh = 1; }

    int bn = snprintf(body, body_cap,
        "{\"model\":\"%s\",\"messages\":%s,\"tools\":%s,\"tool_choice\":\"auto\"}",
        cfg->model[0] ? cfg->model : "gpt-4o-mini", messages_json, tools);

    int ok = 0;
    int r = bsdr_http_request("POST", cfg->endpoint, h, nh, "application/json", body, (size_t)bn, resp, resp_cap);
    if (r < 0) { BSDR_ERROR("bsdr.llm", "complete: request failed"); }
    else if (bsdr_http_status(resp) / 100 != 2) { BSDR_ERROR("bsdr.llm", "complete: HTTP %d", bsdr_http_status(resp)); }
    else {
        const char *bdy = bsdr_http_body(resp);
        if (bdy && extract_json_value(bdy, "message", out, cap)) ok = 1;
    }
    free(body); free(resp);
    return ok;
}

int bsdr_llm_detect_context(const bsdr_llm_config *cfg) {
    if (!cfg || !cfg->endpoint[0]) return 0;
    /* Derive the /models URL from the chat-completions endpoint. */
    char url[300];
    snprintf(url, sizeof url, "%s", cfg->endpoint);
    char *cc = strstr(url, "/chat/completions");
    if (cc) {
        snprintf(cc, sizeof url - (size_t)(cc - url), "/models");
    } else {
        size_t l = strlen(url);
        if (l && url[l - 1] == '/') snprintf(url + l, sizeof url - l, "models");
        else                        snprintf(url + l, sizeof url - l, "/models");
    }

    char auth[320]; bsdr_http_header h[1]; int nh = 0;
    if (cfg->token[0]) { snprintf(auth, sizeof auth, "Bearer %s", cfg->token);
                         h[0].name = "Authorization"; h[0].value = auth; nh = 1; }

    size_t rc = 256 * 1024;
    char *resp = malloc(rc);
    if (!resp) return 0;
    int ctx = 0;
    int r = bsdr_http_request("GET", url, h, nh, NULL, NULL, 0, resp, rc);
    if (r >= 0 && bsdr_http_status(resp) / 100 == 2) {
        const char *bdy = bsdr_http_body(resp);
        if (bdy) {
            /* Prefer the field near our model's id; else the first context field anywhere. */
            const char *scan = bdy;
            if (cfg->model[0]) { const char *m = strstr(bdy, cfg->model); if (m) scan = m; }
            static const char *keys[] = { "context_length", "max_context_length", "context_window",
                                          "max_context_window_tokens", "context_size" };
            long v = -1;
            for (size_t i = 0; i < sizeof keys / sizeof keys[0] && v <= 0; i++) v = json_int_field(scan, keys[i]);
            if (v <= 0 && scan != bdy)
                for (size_t i = 0; i < sizeof keys / sizeof keys[0] && v <= 0; i++) v = json_int_field(bdy, keys[i]);
            if (v > 0) ctx = (int)v;
        }
    }
    free(resp);
    if (ctx > 0) BSDR_INFO("bsdr.llm", "detected context window: %d tokens (%s)", ctx, cfg->model);
    return ctx;
}
