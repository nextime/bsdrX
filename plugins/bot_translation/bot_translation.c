/*
 * bsdrX plugin: bot-translation — translate text/speech for the in-room bot.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>.
 *
 * This bot-translation plugin is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version. See the LICENSE file in this directory.
 *
 * A FEATURE plugin for the bot (deps=["fullbot"]). Registers a `translate` tool in its own TRANSLATE
 * group, backed entirely by the host's LLM service — so when the fullbot loop is running, a speaker can
 * say "bot, translate 'good morning' to Japanese" or "translate what I just said to Spanish" and the
 * model calls this tool. Optionally speaks the result into the room. Uses only public host services
 * (llm_complete / speak / json). See PLAN-bot-plugin.md (P3).
 */
#include "bsdr/plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const bsdr_plugin_host *host;
    int registered;                 /* is the translate tool currently in the registry? */
} bt_state;

#define BT_NAME "bot-translation"

/* Config. Until now this plugin had none: a speaker had to name the target language every single time,
 * and "say it aloud" was only ever whatever the model chose to pass. Both are settings, so they are
 * declared here and the tool falls back to them. */
static const bsdr_plugin_cfgvar BT_CONFIG[] = {
    { .key = "enabled",  .label = "Enable translation", .type = "bool", .def = "1",
      .help = "Off removes the translate tool from the bot entirely" },
    { .key = "language", .label = "Default language", .type = "text", .def = "English",
      .help = "Translate into this when the speaker doesn't name a language" },
    { .key = "speak",    .label = "Speak translations", .type = "bool", .def = "1",
      .help = "Say the translation into the room, not just return it to the bot" },
};

/* Escape text for an HTML attribute. */
static void bt_html_esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *q = src ? src : ""; *q && o + 7 < cap; q++) {
        switch (*q) {
            case '&':  o += (size_t)snprintf(dst + o, cap - o, "&amp;");  break;
            case '<':  o += (size_t)snprintf(dst + o, cap - o, "&lt;");   break;
            case '>':  o += (size_t)snprintf(dst + o, cap - o, "&gt;");   break;
            case '"':  o += (size_t)snprintf(dst + o, cap - o, "&quot;"); break;
            case '\'': o += (size_t)snprintf(dst + o, cap - o, "&#39;");  break;
            default:   dst[o++] = *q;                                     break;
        }
    }
    dst[o < cap ? o : cap - 1] = '\0';
}

/* The configured default language ("English" if unset — config_get has no notion of .def). */
static void bt_language(const bsdr_plugin_host *h, char *out, size_t cap) {
    out[0] = '\0';
    if (h->config_get) h->config_get(BT_NAME, "language", out, cap);
    if (!out[0]) snprintf(out, cap, "English");
}

/* Config is read at use time: the ABI has no config-changed callback, so a cached copy would go stale
 * the moment the operator ticks the box. */
static int bt_flag(const bsdr_plugin_host *h, const char *key, int def) {
    char v[16] = "";
    if (h->config_get) h->config_get(BT_NAME, key, v, sizeof v);
    if (!v[0]) return def;                     /* unset -> the declared default */
    return (v[0] == '1' || v[0] == 't');
}

static int bt_speak_default(const bsdr_plugin_host *h) { return bt_flag(h, "speak", 1); }

/* translate {"text":"…","to":"French","speak":true?} -> {"ok":true,"translation":"…"} */
static int bt_translate(void *state, const char *args_json, int caller_level, char *out, size_t cap) {
    (void)caller_level;
    bt_state *s = (bt_state *)state;
    const bsdr_plugin_host *h = s->host;
    char text[2048] = "", to[64] = "";
    if (h->json_get_str) { h->json_get_str(args_json, "text", text, sizeof text);
                           h->json_get_str(args_json, "to", to, sizeof to); }
    if (!bt_flag(h, "enabled", 1)) {              /* the tool is normally unregistered when off; this
                                                   * covers the window before a sync notices */
        snprintf(out, cap, "{\"ok\":false,\"error\":\"translation is disabled\"}");
        return 1;
    }
    if (!to[0]) bt_language(h, to, sizeof to);     /* "translate that" with no language named */
    /* speak: an explicit false from the model wins; silence means "use the configured default" */
    int say = args_json && strstr(args_json, "\"speak\":false") ? 0
            : (args_json && strstr(args_json, "\"speak\":true")) ? 1
            : bt_speak_default(h);
    if (!text[0] || !h->llm_complete || !h->json_escape) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"need 'text', and an LLM endpoint\"}");
        return 1;
    }

    /* One LLM round-trip (no tools): a translator system prompt + the text. The agentic loop stays in
     * fullbot; this is just a scoped translation call. */
    char sysmsg[256];
    snprintf(sysmsg, sizeof sysmsg,
             "You are a translator. Translate the user's text into %s. Output ONLY the translation.", to);
    char syse[512], te[2200];
    h->json_escape(syse, sizeof syse, sysmsg);
    h->json_escape(te, sizeof te, text);
    char msgs[3200];
    snprintf(msgs, sizeof msgs,
             "[{\"role\":\"system\",\"content\":\"%s\"},{\"role\":\"user\",\"content\":\"%s\"}]", syse, te);

    char msg[4096];
    if (!h->llm_complete(msgs, "[]", msg, sizeof msg)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"translation request failed\"}");
        return 1;
    }
    char tr[2048] = "";
    if (h->json_get_str) h->json_get_str(msg, "content", tr, sizeof tr);
    if (say && tr[0] && h->speak) h->speak(tr);       /* say it into the room too, if asked */
    char tre[2200]; h->json_escape(tre, sizeof tre, tr);
    snprintf(out, cap, "{\"ok\":true,\"translation\":\"%s\"}", tre);
    return 1;
}

static void bt_register(bt_state *s) {
    if (!s->host->tool_register) return;
    s->host->tool_register("translate",
        "Translate text into another language (optionally say it aloud). Use the text of what a "
        "speaker said when they ask to translate it.",
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},"
        "\"to\":{\"type\":\"string\",\"description\":\"target language; omitted = the configured default\"},"
        "\"speak\":{\"type\":\"boolean\"}},\"required\":[\"text\"]}",
        BSDR_PLUGIN_TG_TRANSLATE, bt_translate, s);
    s->registered = 1;
}

/* Disabled means the bot doesn't get the tool at all, not that it calls it and is refused — an offered
 * tool costs prompt tokens and invites a pointless round-trip. The ABI has no config-changed callback,
 * so the flag is picked up on the next status poll (sections_html, below) or at load; bt_translate's own
 * check covers the gap. The registry takes its own lock, so touching it from the web thread is safe. */
static void bt_sync(bt_state *s) {
    int want = bt_flag(s->host, "enabled", 1);
    if (want == s->registered) return;
    if (want) bt_register(s);
    else if (s->host->tool_unregister) { s->host->tool_unregister("translate"); s->registered = 0; }
    s->host->log(1, "bsdr.plugin." BT_NAME, "translate tool %s", want ? "enabled" : "disabled");
}

static int bt_init(const bsdr_plugin_host *host, void **state) {
    bt_state *s = (bt_state *)calloc(1, sizeof *s);
    if (!s) return 1;
    s->host = host;
    if (bt_flag(host, "enabled", 1)) bt_register(s);
    host->log(1, "bsdr.plugin." BT_NAME, "loaded — translate tool %s",
              s->registered ? "registered" : "off (disabled in settings)");
    *state = s;
    return 0;
}

/* One section in the BOT card, not a card of its own: this is a bot feature, and a lone settings card
 * floating next to the bot card is exactly the split we removed from fullbot. The <style> drops the
 * settings card the host would auto-render for our declared config — the fields are here instead. */
static void bt_sections_html(void *state, char *buf, size_t cap, size_t *len) {
    bt_state *s = (bt_state *)state;
    bt_sync(s);
    char lang[64]; bt_language(s->host, lang, sizeof lang);
    char lang_e[160]; bt_html_esc(lang_e, sizeof lang_e, lang);
    int on = bt_flag(s->host, "enabled", 1), say = bt_speak_default(s->host);
    int n = snprintf(buf, cap,
        "<div data-slot=\"bot\">"
        "<style>[data-plugin-config=\"" BT_NAME "\"]{display:none}</style>"
        "<div class=sub2>"
        "<div class=t>Translation <span class=badge>%s</span></div>"
        "<div class=hint>Gives the bot a <b>translate</b> tool: a speaker can say "
        "&ldquo;bot, translate that to Japanese&rdquo;. Off removes the tool from the bot entirely.</div>"
        "<div class=row><label style=width:auto><input type=checkbox data-pcfg-plugin=\"" BT_NAME "\" "
        "data-pcfg-key=enabled style=width:auto%s> enable translation</label></div>"
        "<div class=row><label style=width:130px;color:var(--muted)>Default language</label>"
        "<input data-pcfg-plugin=\"" BT_NAME "\" data-pcfg-key=language name=bsdr-bt-lang autocomplete=off "
        "value=\"%s\" placeholder=English style=max-width:150px>"
        "<span class=hint style=margin-left:8px>used when the speaker doesn&rsquo;t name one</span></div>"
        "<div class=row><label style=width:auto><input type=checkbox data-pcfg-plugin=\"" BT_NAME "\" "
        "data-pcfg-key=speak style=width:auto%s> speak translations into the room</label></div>"
        "</div></div>",
        on ? "on" : "off", on ? " checked" : "", lang_e, say ? " checked" : "");
    *len = (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void bt_shutdown(void *state) { free(state); }

static const char *const BT_DEPS[] = { "fullbot" };

static const bsdr_plugin BOT_TRANSLATION = {
    .abi         = BSDR_PLUGIN_ABI,
    .abi_max     = 0,
    .struct_size = sizeof(bsdr_plugin),
    .name        = "bot-translation",
    .version     = "0.1.0",
    .description = "Translation tool for the fullbot in-room bot (its own tool group)",
    .init        = bt_init,
    .shutdown    = bt_shutdown,
    .sections_html = bt_sections_html,   /* a section in the bot card — no card of our own */
    .config      = BT_CONFIG,
    .config_count = (int)(sizeof BT_CONFIG / sizeof BT_CONFIG[0]),
    .deps        = BT_DEPS,
    .dep_count   = 1,
};

const bsdr_plugin *bsdr_plugin_register(void) { return &BOT_TRANSLATION; }
