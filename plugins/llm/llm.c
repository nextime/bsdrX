/*
 * bsdrX plugin: llm — language-model (endpoint/model/token + context/compaction) settings.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version. See the LICENSE file in this directory.
 *
 * UI-only. The LLM config lives in the CORE (bsdr_app.llm_*) because roomcmd, the owner voice-command
 * balloon and the in-room bot all read it. This plugin owns just the settings CARD, which drives the
 * core /api/voice (llm fields) and /api/llmctx endpoints via window.bsdrUI. It is a reusable base
 * service: the fullbot and bot-computer-control plugins both declare a dependency on it.
 */
#include "bsdr/plugin.h"

#include <stdlib.h>
#include <string.h>

#define LLM_NAME "llm"

typedef struct { const bsdr_plugin_host *host; } llm_state;

/* Persistent UI (declared as .ui_script): builds the LLM settings card and drives the core LLM
 * endpoints via the page's window.bsdrUI helper. Idempotent (guards on its own id). */
/* The token box is type=password, so a browser scans back for the "username" that must go with it and
 * lands on the model box — then autofills it with the saved Bigscreen login e-mail, silently replacing
 * the model id. autocomplete=new-password marks the token as not-a-login-password, which stops that
 * hunt; the distinct name= keeps the model/endpoint from matching any e-mail/username heuristic
 * (autocomplete=off alone is advisory and browsers ignore it on fields they think are credentials). */
static const char LLM_UI_JS[] =
"(function(){if(document.getElementById('llmCard'))return;\n"
"var api=window.bsdrUI.api;\n"
"var c=document.createElement('div');c.className='card';c.id='llmCard';\n"
"c.innerHTML='<h2>LLM</h2>'\n"
"+'<div class=row><input id=fble name=bsdr-llm-endpoint autocomplete=off class=grow placeholder=\"https://api.openai.com/v1/chat/completions\"></div>'\n"
"+'<div class=row><input id=fblm name=bsdr-llm-model autocomplete=off placeholder=gpt-4o-mini style=width:150px><input id=fblt name=bsdr-llm-token type=password autocomplete=new-password placeholder=token style=width:180px></div>'\n"
"+'<div class=hint>Required for spoken <i>commands</i> (the model decides which desktop action to run). Any OpenAI-compatible chat endpoint works. Without it, speech is only typed out.</div>'\n"
"+'<div class=row style=margin-top:8px><label style=width:auto>Context</label><input id=fblctx type=number min=0 step=1024 style=width:110px placeholder=auto><span id=fblctxeff class=hint>tokens (0 = auto-detect from the endpoint, else 32k)</span></div>'\n"
"+'<div class=row><label style=\"width:auto;font-weight:400\"><input id=fblcmpon type=checkbox style=width:auto> compact at</label><input id=fblcmpp type=number min=10 max=100 step=5 style=width:64px> % of context, strategy <select id=fblcmps style=width:auto><option value=0>truncate (drop oldest)</option><option value=1>summarize</option><option value=2>hybrid (summary + recent)</option></select></div>'\n"
"+'<div class=row><label style=width:auto>Max rounds</label><input id=fblrounds type=number min=0 max=200 step=1 style=width:80px placeholder=24><span class=hint>tool-call steps before it stops (higher = deeper coding tasks)</span></div>'\n"
"+'<div class=hint>Compaction shrinks the running conversation once it nears the window so a full multi-step coding session does not overflow — <i>summarize</i>/<i>hybrid</i> cost one extra call but keep the gist; <i>truncate</i> is free but forgets the middle.</div>';\n"
"(document.getElementById('pluginpanels')||document.body).appendChild(c);\n"
"var $=function(id){return document.getElementById(id);};\n"
"function cfg(){api('/api/voice',{llm:$('fble').value,llmModel:$('fblm').value,llmToken:$('fblt').value});}\n"
"function ctxSet(){api('/api/llmctx',{context:+$('fblctx').value||0,compactOn:$('fblcmpon').checked?1:0,compactPct:+$('fblcmpp').value||80,strategy:+$('fblcmps').value||0,maxRounds:+$('fblrounds').value||0});}\n"
"function ctxLoad(){fetch('/api/llmctx').then(function(r){return r.json();}).then(function(d){\n"
"if(document.activeElement!=$('fblctx'))$('fblctx').value=d.context||'';\n"
"if(document.activeElement!=$('fblcmpp'))$('fblcmpp').value=(d.compactPct!=null?d.compactPct:80);\n"
"if(document.activeElement!=$('fblcmps'))$('fblcmps').value=(d.strategy!=null?d.strategy:1);\n"
"if(document.activeElement!=$('fblrounds'))$('fblrounds').value=d.maxRounds||'';\n"
"$('fblcmpon').checked=!!d.compactOn;\n"
"$('fblctxeff').textContent='tokens (0 = auto; effective '+(d.effective||32768)+(d.detected?' detected':'')+')';\n"
"}).catch(function(){});}\n"
"$('fble').onchange=cfg;$('fblm').onchange=cfg;$('fblt').onchange=cfg;\n"
"$('fblctx').onchange=ctxSet;$('fblcmpon').onchange=ctxSet;$('fblcmpp').onchange=ctxSet;$('fblcmps').onchange=ctxSet;$('fblrounds').onchange=ctxSet;\n"
"window.bsdrUI.onStatus(function(s){if(!s||!s.voice)return;\n"
"if(document.activeElement!=$('fble'))$('fble').value=s.voice.llm||'';\n"
"if(document.activeElement!=$('fblm'))$('fblm').value=s.voice.llmModel||'';\n"
"$('fblt').placeholder=s.voice.llmToken?'(set)':'token';\n"
"var lf=!!s.voice.llm;window.bsdrUI.badge('llmCard',lf,lf?'configured':'not set');});\n"
"ctxLoad();})();\n"
;

static int llm_init(const bsdr_plugin_host *host, void **state) {
    llm_state *s = calloc(1, sizeof *s);
    if (!s) return 1;
    s->host = host;
    *state = s;
    return 0;
}

static void llm_shutdown(void *state) { free(state); }

static int llm_http(void *state, const char *method, const char *path, const char *body, void *conn) {
    llm_state *s = (llm_state *)state;
    (void)method; (void)body;
    if (strstr(path, "/ui.js")) {
        s->host->http_respond(conn, 200, "application/javascript", LLM_UI_JS, sizeof LLM_UI_JS - 1);
        return 1;
    }
    return 0;
}

static const bsdr_plugin LLM_PLUGIN = {
    .abi         = BSDR_PLUGIN_ABI,
    .abi_max     = 0,
    .struct_size = sizeof(bsdr_plugin),
    .name        = LLM_NAME,
    .version     = "0.1.0",   /* keep in sync with plugins/llm/VERSION */
    .description = "Language-model endpoint + context/compaction settings (base service)",
    .init        = llm_init,
    .shutdown    = llm_shutdown,
    .http        = llm_http,
    .ui_script   = "/api/plugin/llm/ui.js",
};

const bsdr_plugin *bsdr_plugin_register(void) { return &LLM_PLUGIN; }
