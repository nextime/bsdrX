/*
 * bsdrX plugin: voice-services — STT + TTS settings for the assistant and the in-room bot.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version. See the LICENSE file in this directory.
 *
 * UI-only. The STT/TTS engine and its config live in the CORE (bsdr_app) because roomcmd, the owner
 * voice-control path and the bot audio producer all read them. This plugin owns just the settings
 * CARD, which drives the core /api/voice, /api/tts and /api/tts/say endpoints via window.bsdrUI. It is
 * a reusable base service: the fullbot plugin declares a dependency on it, and future plugins can too.
 */
#include "bsdr/plugin.h"

#include <stdlib.h>
#include <string.h>

#define VS_NAME "voice-services"

typedef struct { const bsdr_plugin_host *host; } vs_state;

/* Persistent UI (declared as .ui_script): builds the Voice services card (STT + TTS) and drives the
 * core voice endpoints via the page's window.bsdrUI helper. Idempotent (guards on its own id). */
static const char VS_UI_JS[] =
"(function(){if(document.getElementById('vsVoiceCard'))return;\n"
"var api=window.bsdrUI.api;var $=function(id){return document.getElementById(id);};\n"
"var c=document.createElement('div');c.className='card';c.id='vsVoiceCard';\n"
/* name= + autocomplete on every field paired with a token box: without them the browser reads the
 * type=password token as a login and autofills the model/voice box next to it with the saved
 * Bigscreen e-mail, wiping what you typed. Same reasoning as the LLM card. */
"c.innerHTML='<h2>Voice services</h2>'\n"
"+'<div class=hint>Speech-to-text and text-to-speech for the assistant and the bot. The <b>LLM</b> is configured in its own <b>LLM</b> card.</div>'\n"
"+'<div class=sub2><div class=t>Speech-to-text (STT)<span id=sttbadge class=\"badge free\">free online service</span></div>'\n"
"+'<div class=row><input id=se name=bsdr-stt-endpoint autocomplete=off class=grow placeholder=\"leave blank = free online service\"></div>'\n"
"+'<div class=row><input id=sm name=bsdr-stt-model autocomplete=off placeholder=whisper-1 style=width:150px><input id=st name=bsdr-stt-token type=password autocomplete=new-password placeholder=token style=width:180px></div>'\n"
"+'<div class=hint id=stthint>Leave the URL blank to use a built-in <b>free online</b> transcription service &mdash; no setup. For private/faster results, point it at your own Whisper server (e.g. <code>http://localhost:8080/inference</code>) or any OpenAI-compatible <code>/v1/audio/transcriptions</code> endpoint, plus its model and token.</div></div>'\n"
"+'<div class=sub2><div class=t>Text-to-speech (TTS) <span class=badge>speak</span><label style=\"margin-left:auto;width:auto;font-weight:400\"><input id=ttson type=checkbox style=width:auto> enable</label></div>'\n"
"+'<div class=row><label style=width:110px;color:var(--muted)>Engine</label><select id=ttseng style=width:auto><option value=0>Local (Piper)</option><option value=1>Cloud</option><option value=2>FreeTTS.org (free)</option></select><label style=\"color:var(--muted);margin-left:12px\">Speak to</label><select id=ttsroute style=width:auto><option value=0>the room (cloud)</option><option value=1>desktop &#8594; Quest</option></select><button id=fbtts_free style=margin-left:auto>Free (no key)</button><button id=fbtts_edge style=margin-left:6px>Edge-TTS</button><button id=fbtts_groq style=margin-left:6px>Groq</button></div>'\n"
"+'<div class=row id=ttslocalrow><input id=ttspiper placeholder=\"piper binary (default: piper on PATH)\" style=max-width:200px><input id=ttsmodel class=grow placeholder=\"Piper voice .onnx path (on the agent host)\"><button id=fbtts_browse>Browse</button></div>'\n"
"+'<div class=row id=ttscloudrow><input id=ttsendp name=bsdr-tts-endpoint autocomplete=off class=grow placeholder=\"https://api.openai.com/v1/audio/speech\"></div>'\n"
"+'<div class=row id=ttsvoicerow><input id=ttscm name=bsdr-tts-model autocomplete=off placeholder=\"tts-1 / playai-tts\" style=max-width:150px><label style=\"color:var(--muted)\">Voice</label><select id=ttsvoicesel style=max-width:180px></select><input id=ttsvoice name=bsdr-tts-voice autocomplete=off placeholder=\"or type a voice\" style=max-width:150px><input id=ttstok name=bsdr-tts-token type=password autocomplete=new-password placeholder=token style=width:150px></div>'\n"
"+'<div class=row id=ttssayrow><input id=ttssaytxt class=grow placeholder=\"say this to the room now&#8230;\"><button id=fbtts_say>Say</button></div>'\n"
"+'<div class=hint id=ttshint>Lets the assistant <b>speak</b> its replies (the <code>speak</code> tool); pick a <b>Voice</b> from the list or type one. <b>Free (no key)</b> = <b>FreeTTS.org</b> &mdash; zero setup, no key (Edge neural voices, 20 req/min). <b>Edge-TTS</b> = best free quality, self-hosted: run <code>docker run -d -p 5050:5050 -e API_KEY=yourkey travisvn/openai-edge-tts</code> then put that key in the token field. <b>Groq</b> = cloud, paste a free Groq key. <b>Local (Piper)</b> is fully offline (a <code>piper</code> binary + a voice <code>.onnx</code>). Speech goes to the <b>room</b> via the bot (others hear it &mdash; needs the bot joined) or to <b>desktop audio</b> so you hear it in the headset. Use <b>Say</b> to make the bot speak a line right now (and to test the path).</div></div>'\n"
"+'<div class=row style=margin-top:12px><button class=p id=fbva_apply>Apply voice settings</button></div>';\n"
"(document.getElementById('pluginpanels')||document.body).appendChild(c);\n"
"function voicecfg(){api('/api/voice',{stt:se.value,sttModel:sm.value,sttToken:st.value});}\n"
"var TTSV={openai:['alloy','echo','fable','onyx','nova','shimmer'],\n"
"edge:['alloy','echo','fable','onyx','nova','shimmer','en-US-AndrewNeural','en-US-AriaNeural','en-US-JennyNeural','en-US-GuyNeural','en-US-EmmaNeural','en-GB-SoniaNeural','en-GB-RyanNeural','it-IT-ElsaNeural','it-IT-DiegoNeural'],\n"
"freetts:['en-US-JennyNeural','en-US-AriaNeural','en-US-GuyNeural','en-US-AndrewNeural','en-US-EmmaNeural','en-GB-SoniaNeural','en-GB-RyanNeural','it-IT-ElsaNeural','it-IT-DiegoNeural','fr-FR-DeniseNeural','de-DE-KatjaNeural','es-ES-ElviraNeural'],\n"
"groq:['Fritz-PlayAI','Aaliyah-PlayAI','Adelaide-PlayAI','Angelo-PlayAI','Arista-PlayAI','Atlas-PlayAI','Basil-PlayAI','Briggs-PlayAI','Calum-PlayAI','Celeste-PlayAI','Cheyenne-PlayAI','Chip-PlayAI','Deedee-PlayAI','Gail-PlayAI','Indigo-PlayAI','Mamaw-PlayAI','Mason-PlayAI','Mikail-PlayAI','Mitch-PlayAI','Quinn-PlayAI','Thunder-PlayAI']};\n"
"function ttsFillVoices(kind,pick){var a=TTSV[kind]||[];ttsvoicesel.innerHTML='<option value=\"\">(pick a voice)</option>'+a.map(function(v){return '<option>'+v+'</option>';}).join('');var cur=pick||ttsvoice.value;if(cur&&a.indexOf(cur)>=0)ttsvoicesel.value=cur;if(pick){ttsvoice.value=pick;ttsvoicesel.value=pick;}}\n"
"function ttsVis(){var e=+ttseng.value;ttslocalrow.style.display=e===0?'flex':'none';ttscloudrow.style.display=e===1?'flex':'none';ttsvoicerow.style.display=(e===1||e===2)?'flex':'none';ttscm.style.display=e===1?'':'none';ttstok.style.display=e===1?'':'none';}\n"
"function ttsEngChange(){ttsVis();ttsFillVoices(+ttseng.value===2?'freetts':'openai');ttsSet();}\n"
"function ttsSet(){ttsVis();api('/api/tts',{on:ttson.checked?1:0,engine:+ttseng.value,route:+ttsroute.value,piper:ttspiper.value||'',model:ttsmodel.value||'',endpoint:ttsendp.value||'',cloudModel:ttscm.value||'',voice:ttsvoice.value||'',token:ttstok.value||''});}\n"
"function ttsSay(){var t=ttssaytxt.value.trim();if(!t)return;if(!ttson.checked){ttson.checked=true;ttsSet();}api('/api/tts/say',{text:t});ttssaytxt.value='';}\n"
"function ttsGroq(){ttseng.value=1;ttsendp.value='https://api.groq.com/openai/v1/audio/speech';if(!ttscm.value)ttscm.value='playai-tts';ttsFillVoices('groq',ttsvoice.value||'Fritz-PlayAI');ttson.checked=true;ttsVis();ttsSet();}\n"
"function ttsEdge(){ttseng.value=1;ttsendp.value='http://localhost:5050/v1/audio/speech';ttscm.value='tts-1';ttsFillVoices('edge',ttsvoice.value||'alloy');ttson.checked=true;ttsVis();ttsSet();}\n"
"function ttsFree(){ttseng.value=2;ttsFillVoices('freetts',ttsvoice.value||'en-US-JennyNeural');ttson.checked=true;ttsVis();ttsSet();}\n"
"function ttsBrowse(){window.fbTarget='ttsmodel';window.fbKind='onnx';var p=ttsmodel.value||'';var d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';window.fbBrowse(d);}\n"
"function renderTTS(t){if(!t)return;if(document.activeElement!==ttson)ttson.checked=!!t.enabled;\n"
"if(document.activeElement!==ttseng)ttseng.value=t.engine;if(document.activeElement!==ttsroute)ttsroute.value=t.route;\n"
"var set=function(el,v){if(document.activeElement!==el&&el.value!==v)el.value=v;};\n"
"set(ttspiper,t.piper||'');set(ttsmodel,t.model||'');set(ttsendp,t.endpoint||'');set(ttscm,t.cloudModel||'');set(ttsvoice,t.voice||'');\n"
"if(ttsvoicesel.dataset.eng!=(''+t.engine+t.endpoint)){var ep=t.endpoint||'';var k=t.engine===2?'freetts':(ep.indexOf('groq')>=0?'groq':(ep.indexOf('5050')>=0?'edge':'openai'));ttsFillVoices(k,'');ttsvoicesel.dataset.eng=(''+t.engine+t.endpoint);}\n"
"if(document.activeElement!==ttsvoicesel)ttsvoicesel.value=t.voice||'';\n"
"if(document.activeElement!==ttstok)ttstok.placeholder=t.tokenSet?'token set — leave blank to keep':'token';ttsVis();}\n"
"se.onchange=voicecfg;sm.onchange=voicecfg;st.onchange=voicecfg;$('fbva_apply').onclick=voicecfg;\n"
"ttson.onchange=ttsSet;ttseng.onchange=ttsEngChange;ttsroute.onchange=ttsSet;\n"
"ttspiper.onchange=ttsSet;ttsmodel.onchange=ttsSet;ttsendp.onchange=ttsSet;ttscm.onchange=ttsSet;ttsvoice.onchange=ttsSet;ttstok.onchange=ttsSet;\n"
"ttsvoicesel.onchange=function(){ttsvoice.value=this.value;ttsSet();};\n"
"ttssaytxt.onkeydown=function(ev){if(ev.key==='Enter')ttsSay();};\n"
"$('fbtts_free').onclick=ttsFree;$('fbtts_edge').onclick=ttsEdge;$('fbtts_groq').onclick=ttsGroq;$('fbtts_browse').onclick=ttsBrowse;$('fbtts_say').onclick=ttsSay;\n"
"window.bsdrUI.onStatus(function(s){if(!s||!s.voice)return;\n"
"if(s.tts)renderTTS(s.tts);\n"
"if(document.activeElement!==se)se.value=s.voice.stt||'';\n"
"if(document.activeElement!==sm)sm.value=s.voice.sttModel||'';\n"
"st.placeholder=s.voice.sttToken?'(set)':'token';\n"
"var sf=!s.voice.stt;sttbadge.textContent=sf?'free online service':'custom endpoint';sttbadge.className='badge '+(sf?'free':'custom');\n"/* STT always works (free fallback), so the badge reports which endpoint is in use, not on/off */
"window.bsdrUI.badge('vsVoiceCard',!sf,sf?'free':'custom');});\n"
"})();\n"
;

static int vs_init(const bsdr_plugin_host *host, void **state) {
    vs_state *s = calloc(1, sizeof *s);
    if (!s) return 1;
    s->host = host;
    *state = s;
    return 0;
}

static void vs_shutdown(void *state) { free(state); }

/* Only serves the persistent management script; STT/TTS config goes to the core endpoints. */
static int vs_http(void *state, const char *method, const char *path, const char *body, void *conn) {
    vs_state *s = (vs_state *)state;
    (void)method; (void)body;
    if (strstr(path, "/ui.js")) {
        s->host->http_respond(conn, 200, "application/javascript", VS_UI_JS, sizeof VS_UI_JS - 1);
        return 1;
    }
    return 0;
}

static const bsdr_plugin VS_PLUGIN = {
    .abi         = BSDR_PLUGIN_ABI,
    .abi_max     = 0,
    .struct_size = sizeof(bsdr_plugin),
    .name        = VS_NAME,
    .version     = "0.1.0",   /* keep in sync with plugins/voice_services/VERSION */
    .description = "STT + TTS settings for the assistant and the in-room bot (base service)",
    .init        = vs_init,
    .shutdown    = vs_shutdown,
    .http        = vs_http,
    .ui_script   = "/api/plugin/voice-services/ui.js",
};

const bsdr_plugin *bsdr_plugin_register(void) { return &VS_PLUGIN; }
