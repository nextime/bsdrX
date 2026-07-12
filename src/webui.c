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
/* Local control web UI (127.0.0.1): page + JSON API over a tiny HTTP server. */
#include "bsdr/webui.h"
#include "bsdr/version.h"
#include "bsdr/net.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/winlist.h"
#include "bsdr/model_store.h"
#include "bsdr/voicestore.h"
#include "bsdr/webcam.h"
#include "bsdr/deps.h"
#include "bsdr/tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ---- file-browser helpers (server-side directory listing for the source picker) ---- */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Decode a %-encoded query value up to '&' or end. */
static void url_decode(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && *p != '&' && o + 1 < cap; p++) {
        int hi, lo;
        if (*p == '%' && (hi = hexv(p[1])) >= 0 && (lo = hexv(p[2])) >= 0) { out[o++] = (char)((hi << 4) | lo); p += 2; }
        else out[o++] = (*p == '+') ? ' ' : *p;
    }
    out[o] = 0;
}
/* case-insensitive suffix match */
static int ci_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    const char *p = s + ls - lf;
    for (size_t i = 0; i < lf; i++) if (((unsigned char)p[i] | 0x20) != ((unsigned char)suf[i] | 0x20)) return 0;
    return 1;
}
/* Show only media containers and .txt playlists in the browser. */
static int is_media_name(const char *n) {
    static const char *ext[] = { ".mp4",".mkv",".mov",".avi",".webm",".m4v",".ts",".h264",
                                 ".264",".flv",".wmv",".mpg",".mpeg",".m2ts",".txt", NULL };
    for (int i = 0; ext[i]; i++) if (ci_suffix(n, ext[i])) return 1;
    return 0;
}

struct bsdr_webui {
    bsdr_app *app;
    bsdr_socket_t listener;
    bsdr_thread *thread;
    volatile int running;
};

static const char PAGE[] =
"<!doctype html><html lang=en><head><meta charset=utf-8><title>bsdrX</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
":root{--bg:#0d0f14;--panel:#161a22;--panel2:#1d222c;--edge:#2a313d;"
"--txt:#e7eaf0;--muted:#9aa4b2;--accent:#5b9dff;--accentd:#3f7fe0;"
"--on:#2ecc71;--off:#ff5d5d;--warn:#f0b429}"
"*{box-sizing:border-box}"
"body{font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
"background:radial-gradient(1200px 600px at 50% -10%,#1a2030,#0d0f14 60%);"
"color:var(--txt);max-width:760px;margin:0 auto;padding:28px 18px 60px;-webkit-font-smoothing:antialiased}"
"header{display:flex;align-items:baseline;gap:12px;margin-bottom:8px}"
"h1{font-size:22px;font-weight:700;letter-spacing:.2px;margin:0}"
"header .sub{color:var(--muted);font-size:13px}"
".card{background:linear-gradient(180deg,var(--panel),var(--panel2));"
"border:1px solid var(--edge);border-radius:14px;padding:16px 18px;margin:14px 0;"
"box-shadow:0 8px 24px rgba(0,0,0,.35)}"
".card h2{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.08em;"
"color:var(--muted);margin:0 0 12px;display:flex;align-items:center;gap:8px}"
".dot{display:inline-block;width:9px;height:9px;border-radius:50%;flex:none;"
"box-shadow:0 0 0 3px rgba(255,255,255,.04)}"
".on{background:var(--on);box-shadow:0 0 8px var(--on)}.off{background:var(--off)}"
".status{display:flex;align-items:center;gap:9px;font-size:14.5px}"
".pill{font-size:11px;color:var(--muted);border:1px solid var(--edge);border-radius:999px;"
"padding:1px 9px;margin-left:auto}"
"label{color:var(--muted);font-size:13px}"
".row{display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin:8px 0}"
".grow{flex:1 1 240px}"
"input{font:14px inherit;color:var(--txt);background:#0f131a;border:1px solid var(--edge);"
"border-radius:9px;padding:9px 11px;outline:none;transition:border-color .15s,box-shadow .15s;width:100%}"
"input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(91,157,255,.18)}"
"input::placeholder{color:#5d6675}"
"input[type=number]{width:auto}"
"button{font:600 14px inherit;color:var(--txt);background:#222a36;border:1px solid var(--edge);"
"border-radius:9px;padding:9px 16px;cursor:pointer;transition:background .15s,transform .05s}"
"button:hover{background:#2b3441}button:active{transform:translateY(1px)}"
"button.p{background:linear-gradient(180deg,var(--accent),var(--accentd));border-color:var(--accentd);color:#fff}"
"button.p:hover{filter:brightness(1.07)}"
"button.danger{background:#3a2230;border-color:#5a2a3a;color:#ffb3c4}"
".hint{color:var(--muted);font-size:12.5px;margin-top:6px}"
".sub2{background:#0f131a;border:1px solid var(--edge);border-radius:11px;padding:13px 14px;margin-top:10px}"
".sub2 .t{font-weight:600;font-size:13px;margin-bottom:9px;display:flex;align-items:center;gap:8px}"
".badge{font-size:11px;font-weight:600;border-radius:999px;padding:2px 9px}"
".badge.free{background:rgba(46,204,113,.14);color:#7ee2a8;border:1px solid rgba(46,204,113,.35)}"
".badge.custom{background:rgba(91,157,255,.14);color:#a9caff;border:1px solid rgba(91,157,255,.35)}"
".q{display:flex;align-items:center;gap:10px;padding:7px 0;border-top:1px solid var(--edge)}"
".q:first-of-type{border-top:0}.q .nm{flex:1}"
"a{color:var(--accent)}"
/* collapsible panels */
".card h2.clp{cursor:pointer;user-select:none;-webkit-user-select:none}"
".card h2 .hdrr{margin-left:auto;display:flex;align-items:center;gap:8px}"
".card h2 .chev{font-size:10px;opacity:.55;transition:transform .18s}"
".card.col h2 .chev{transform:rotate(-90deg)}"
".card.col>:not(h2){display:none!important}"   /* !important beats JS-set inline display on rows */
".card h2 .en{font-size:10.5px;font-weight:700;letter-spacing:.02em;text-transform:none;"
"border-radius:999px;padding:2px 9px;border:1px solid var(--edge);color:var(--muted);white-space:nowrap}"
".card h2 .en.y{color:var(--on);border-color:rgba(46,204,113,.45);background:rgba(46,204,113,.12)}"
".card h2 .en.n{opacity:.65}"
".card h2 .en:empty{display:none}"
/* phone layout */
"@media(max-width:560px){"
"body{padding:16px 11px 44px}"
".card{padding:13px 13px;margin:11px 0;border-radius:12px}"
".card h2{margin-bottom:10px;flex-wrap:wrap;row-gap:5px}"
"h1{font-size:19px}header{flex-wrap:wrap;gap:6px 10px}"
".row{gap:8px;margin:7px 0}.grow{flex:1 1 140px}"
"label{font-size:12.5px}button{padding:10px 15px}"
"select,input{max-width:100%}"
".card h2 .en{font-size:10px;padding:2px 7px}}"
"</style></head><body>"
"<header><h1>bsdrX <small style='font-size:.5em;opacity:.55;font-weight:400'>v" BSDR_VERSION "</small></h1><span class=sub>Bigscreen Remote Desktop</span>"
"<a href='https://bigscreen.nexlab.net' target=_blank rel=noopener style=margin-left:auto>bigscreen.nexlab.net</a>"
"<button class=p style=width:auto onclick=showDonate()>&#9829; Donate</button></header>"

"<div class=card><h2>Bigscreen account</h2>"
"<div id=cloud class=status>...</div>"
"<div id=loginform class=row style=margin-top:12px>"
"<input id=email class=grow placeholder='Bigscreen email'> "
"<input id=pw class=grow type=password placeholder=password> "
"<button class=p onclick=login()>Log in</button></div>"
"<div id=loginform2 class=row><label style=width:auto;color:var(--muted)><input id=tlsinsecure type=checkbox style=width:auto onchange=tlsToggle()> "
"Disable TLS certificate verification (insecure — only for a broken/MITM'd CA store)</label></div>"
"<div id=cloudshare class=row style='margin-top:12px;display:none'>"
"<span id=sharelbl class=grow>Internet sharing: off</span>"
"<button id=sharebtn class=p onclick=toggleShare()>Share to Internet</button></div>"
"<div class=row id=blankrow><label style=width:auto><input id=blank type=checkbox style=width:auto onchange=blankToggle()> "
"Blank my physical screen while the headset is connected (privacy)</label></div>"
"<div class=row><label style=width:auto><input id=ptouch type=checkbox style=width:auto onchange=pointerModeToggle()> "
"Use the headset pointer as a <b>touchpad</b> (real tap/drag touch events) instead of a mouse</label></div></div>"

/* Second (bot) account — its own login + room-join. Joining your room as a 2nd participant makes the
 * headset send your owner mic even when you're otherwise alone (Room.participants > 1). */
"<div class=card><h2>Second account (bot) <span class=badge>experimental</span></h2>"
"<div class=hint>Log a <b>second, different</b> Bigscreen account in here and hit <b>Join my room</b> — "
"the headset then sees <b>more than one participant</b> and sends your owner mic even when you're alone. "
"<b>Join my room</b> is smart: if the room is open it joins directly; if it's friends/verified/invite-only "
"it makes your host account <b>invite</b> the bot, the bot <b>accepts</b>, then joins — <b>without</b> changing "
"your room's privacy (only a fully-closed room is minimally opened to invite-only). Its session is separate "
"from your main account (own token; password never stored). Note: an invite may require the two accounts to be <b>friends</b> first.</div>"
"<div id=bot class=status style=margin-top:8px>...</div>"
"<div id=botlogin class=row style=margin-top:10px>"
"<input id=botemail class=grow placeholder='bot Bigscreen email'> "
"<input id=botpw class=grow type=password placeholder=password> "
"<button class=p onclick=botLogin()>Log in</button></div>"
"<div id=botstart class=row style='margin-top:10px;display:none'>"
"<button class=p onclick=botStart()>Start bot</button>"
"<button class=danger style=margin-left:auto onclick=botLogout()>Forget login</button></div>"
"<div id=botactions class=row style='margin-top:8px;display:none'>"
"<button id=botjoinbtn class=p onclick=botJoin()>Join my room</button>"
"<button id=botleavebtn onclick=botLeave()>Leave room</button>"
"<label class=hint style='margin-left:12px'>Presence "
"<select id=botmode onchange=botMode()>"
"<option value=audio>audio only (unlock mic)</option>"
"<option value=full>full bot (avatar)</option></select></label>"
"<button onclick=botStop()>Stop</button>"
"<button class=danger style=margin-left:8px onclick=botLogout()>Log out</button></div>"
"<div id=botfollowrow class=row style='margin-top:8px;display:none'>"
"<label class=hint><input type=checkbox id=botfollow onchange=botFollowSet()> "
"<b>Follow me into rooms</b> — the bot re-joins whenever you move to another room</label></div>"
"<div id=botlooprow class=row style='margin-top:6px;display:none'>"
"<label class=hint><input type=checkbox id=botloop onchange=botLoopSet()> "
"<b>Cloud-mic loopback</b> — route the bot's room audio into BSDR_RoomMic (adds your OWN voice; works "
"solo)</label>"
"<label class=hint style='margin-left:16px'><input type=checkbox id=botsolo onchange=botSoloSet()> "
"listen only to me</label></div>"
"<div class=hint style=margin-top:6px><b>audio only</b> just sits in the room so your mic works when "
"you're alone — no avatar. <b>full bot</b> also shows an avatar in the room (needed for in-room "
"moderation).</div></div>"

"<div class=card><h2>Headset (Quest)</h2>"
"<div id=quest class=status>...</div>"
"<div id=quests style=margin-top:6px></div>"
"<div class=hint>Pairing is automatic — the headset finds this PC on the LAN. "
"No pairing code to type.</div></div>"

"<div class=card><h2>Headset mic</h2>"
"<div id=sniff class=status>...</div>"
#if defined(__ANDROID__)
"<div class=hint>The Quest never sends its mic to this device — the headset owner's voice only goes to "
"the Bigscreen room (cloud). A <b>router companion</b> (<b>bsdr_micrelay</b>) on your router intercepts "
"that stream and forwards it here (<b>Relay</b>), where it's exposed as a virtual microphone "
"<b>BSDR_QuestMic</b>. Set the relay port below.</div>"
#else
"<div class=hint>The Quest never sends its mic to this PC — the headset owner's voice only goes to the "
"Bigscreen room (cloud). This intercepts that stream off the LAN and exposes it as a virtual "
"microphone <b>BSDR_QuestMic</b>. <b>Sniff</b> (passive) needs this PC to see the headset's traffic "
"(be the gateway or a mirror port); <b>MITM</b> ARP-reroutes it through this PC on a switched LAN; "
"<b>Relay</b> takes the mic from a router companion. Sniff/MITM need the root password (used once "
"for a helper, never stored).</div>"
#endif
"<div class=row><label style=width:auto;color:var(--muted)>Method</label>"
"<select id=snmethod onchange=snMethodChange() style=width:auto>"
#if !defined(__ANDROID__)
/* Sniff & MITM both need local packet capture — impossible on Android (no CAP_NET_RAW, and Wi-Fi
 * managed mode can't see another station); Android's owner mic is relay-only. */
"<option value=0>Sniff (passive)</option>"
"<option value=1>MITM (ARP)</option>"
#endif
"<option value=2>Relay (router)</option></select></div>"
"<div class=row id=relayrow style=display:none><label style=width:auto;color:var(--muted)>Router relay port</label>"
"<input id=relayport type=number min=0 max=65535 placeholder='e.g. 45099 (bsdr_micrelay)' onchange=relayPortSet() style=width:12em>"
"<span class=hint style=margin-left:8px>the router companion forwards the mic here — <b>required on Android</b> (no local sniff)</span></div>"
"<div class=row>"
#if !defined(__ANDROID__)
/* sniff/MITM run a root helper; Android is relay-only (no capture) so it needs no password */
"<input id=snpw type=password class=grow placeholder='root (sudo) password — only if not already root'>"
#endif
"<button id=snbtn class=p onclick=toggleSniff()>Start mic</button></div>"
#if defined(__ANDROID__)
"<div class=hint>Run the <b>router companion</b> <b>bsdr_micrelay</b> on your router — it captures the "
"headset's uplink there and forwards it here (set the <b>Relay</b> port above). Prebuilt binaries for "
"common routers are in <b>bsdrX_relay.zip</b>; see bsdr_micrelay(1).</div>"
#else
"<div class=hint><b>On Wi-Fi</b>, MITM works <i>unless</i> the AP enforces <b>client isolation</b> "
"(a.k.a. AP/station isolation) — bsdrX NATs the headset's uplink so the AP's source-guard doesn't "
"drop it. If isolation is on, clients can't reach each other and neither MITM nor Sniff can see the "
"headset; run the <b>router companion</b> <b>bsdr_micrelay</b> on your router instead (<b>Relay</b> "
"method, or start bsdrX with <code>--sniff-remote PORT</code>). Prebuilt binaries for common routers "
"are in <b>bsdrX_relay.zip</b>; see bsdr_micrelay(1).</div>"
#endif
/* realtime voice changer on the Quest mic */
"<div class=sub2><div class=t>Voice changer &#8212; effects <span class=badge>realtime</span>"
"<label style='margin-left:auto;width:auto;font-weight:400'><input id=vfxon type=checkbox style=width:auto onchange=voicefx()> enable</label></div>"
"<div class=hint style=margin-bottom:6px>Your mic runs through <b>one</b> changer with two engines: these <b>DSP effects</b>, "
"or the <b>AI voice (RVC)</b> below &#8212; turning on <b>use AI</b> overrides these effects (only <b>Volume</b> carries over). "
"<b>Substitute</b> (bottom) then decides whether the room/Quest hears the changed voice or just your local virtual mic; it "
"applies to whichever engine is on.</div>"
"<div class=row style='flex-wrap:wrap;gap:4px'><label style=width:120px;color:var(--muted)>Presets</label>"
"<button onclick=\"vpreset('feminize')\">Feminize</button>"
"<button onclick=\"vpreset('masculinize')\">Masculinize</button>"
"<button onclick=\"vpreset('younger')\">Younger</button>"
"<button onclick=\"vpreset('older')\">Older</button>"
"<button onclick=\"vpreset('chipmunk')\">Chipmunk</button>"
"<button onclick=\"vpreset('deep')\">Deep</button>"
"<button onclick=\"vpreset('robot')\">Robot</button>"
"<button onclick=\"vpreset('reset')\">Reset</button></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Pitch</label>"
"<input id=vgender type=range min=-100 max=100 value=0 oninput='vgv.textContent=vgender.value' onchange=voicefx() class=grow>"
"<span id=vgv style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Formant (tone)</label>"
"<input id=vformant type=range min=-100 max=100 value=0 oninput='vfmv.textContent=vformant.value' onchange=voicefx() class=grow>"
"<span id=vfmv style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Volume</label>"
"<input id=vvolume type=range min=-100 max=100 value=0 oninput='vvv.textContent=vvolume.value' onchange=voicefx() class=grow>"
"<span id=vvv style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Robot</label>"
"<input id=vrobot type=range min=0 max=100 value=0 oninput='vrv.textContent=vrobot.value' onchange=voicefx() class=grow>"
"<span id=vrv style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Echo</label>"
"<input id=vecho type=range min=0 max=100 value=0 oninput='vev.textContent=vecho.value' onchange=voicefx() class=grow>"
"<span id=vev style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Whisper</label>"
"<input id=vwhisper type=range min=0 max=100 value=0 oninput='vwv.textContent=vwhisper.value' onchange=voicefx() class=grow>"
"<span id=vwv style=width:2.5em;text-align:right>0</span></div>"
"<div class=hint>Live, no model, all platforms. <b>Enable</b> turns the changer on (the sliders keep "
"their positions when off). <b>Pitch</b> shifts pitch+formants (&#8722; deeper / + higher); "
"<b>Formant</b> tilts the tone brighter/darker (vocal-tract size); <b>Volume</b> = output gain; "
"<b>Robot</b> = ring-mod timbre; <b>Echo</b> = trailing echo; <b>Whisper</b> = breathy. The changer "
"always feeds the host virtual mic (BSDR_QuestMic); the room/Quest only hears it when <b>Substitute</b> "
"below is ticked.</div>"
"<div class=row><label style=width:auto><input id=vsub type=checkbox style=width:auto onchange=voicefx()> "
#if defined(__ANDROID__)
"Substitute into the cloud (relay): stop the headset&#8217;s original voice and inject the changed (or AI-converted) audio</label></div>"
"<div class=hint>Requires <b>Relay</b> active (we rewrite the headset&#8594;cloud packets in "
#else
"Substitute into the cloud (MITM/relay): stop the headset&#8217;s original voice and inject the changed (or AI-converted) audio</label></div>"
"<div class=hint>Requires <b>MITM</b> or <b>Relay</b> active (we rewrite the headset&#8594;cloud packets in "
#endif
"flight via NFQUEUE) and the agent running with <b>root / CAP_NET_ADMIN</b> on Linux. Otherwise the change "
"is still heard on the local virtual mic and the cloud-room fallback.</div></div>"
/* AI voice conversion (RVC) — the model-based tier behind the same changer */
"<div class=sub2><div class=t>Voice changer &#8212; AI voice (RVC) <span class=badge>model</span>"
"<label style='margin-left:auto;width:auto;font-weight:400'><input id=vaion type=checkbox style=width:auto onchange=voiceai()> use AI</label></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Quality</label>"
"<select id=vaitier onchange=voiceai()><option value=1>CPU</option><option value=2>Small GPU</option><option value=3>Big GPU</option></select>"
"<label style='color:var(--muted);margin-left:12px'>Voice</label>"
"<select id=vaivoice onchange=voiceai() class=grow></select></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Pitch key</label>"
"<input id=vaikey type=range min=-24 max=24 value=0 oninput='vaikv.textContent=vaikey.value' onchange=voiceai() class=grow>"
"<span id=vaikv style=width:2.5em;text-align:right>0</span></div>"
"<div class=hint id=vaistat>&#8212;</div>"
"<div class=row><button onclick=vaidl()>Download engine models</button><span id=vaidls class=hint></span></div>"
"<div class=row><input id=vaizip placeholder='engine models .zip path' class=grow><button onclick=vaiimp()>Import zip</button></div>"
"<div class=t style=margin-top:8px>Add a voice"
"<a href='https://huggingface.co/models?search=rvc+onnx' target=_blank rel=noopener style='margin-left:8px;font-weight:400;font-size:12px'>find voices online &#8599;</a></div>"
"<div class=row><input id=vaddid placeholder=id style=max-width:120px><input id=vaddname placeholder='display name' class=grow></div>"
"<div class=row><input id=vaddurl placeholder='voice .onnx URL (download online)' class=grow><button onclick=vaiAddUrl()>Download</button></div>"
"<div class=row><input id=vaddpath placeholder='local .onnx path' class=grow><button onclick=\"vaddBrowse()\">Browse</button><button onclick=vaiAddFile()>Add file</button></div>"
"<div id=vailist class=hint style=margin-top:4px></div>"
"<div class=t style=margin-top:8px>Voice presets <span class=badge>up to 5</span></div><div id=vaipresets></div>"
"<div class=hint>Converts your mic to the selected <b>voice</b> (an RVC <code>.onnx</code> you supply or download online &#8212; "
"use <b>find voices online</b> above; note many community voices ship as <code>.pth</code> and must be exported to ONNX first). "
"<b>Quality</b> = CPU / small-GPU / big-GPU (ONNX Runtime; falls back to CPU everywhere, Android included). Needs the "
"engine base models once (Download or Import). When <b>use AI</b> is on it <b>replaces the DSP effects</b> above "
"(Pitch/Formant/Robot/Echo/Whisper are bypassed) &#8212; only <b>Volume</b> and <b>Substitute</b> from the changer still apply. "
"<b>Presets</b> save the whole changer state (AI + DSP knobs) to a named slot.</div></div>"
/* Room mic: the OTHER participants' voices, consumed from the cloud room's SFU (micPort). Distinct
 * from BSDR_QuestMic (the headset owner's own voice, sniffed off the LAN). Needs an active cloud
 * session (Internet sharing on). */
"<div class=sub2><div class=t>Room mic <span class=badge>cloud</span>"
"<label style='margin-left:auto;width:auto;font-weight:400'><input id=roommic type=checkbox style=width:auto onchange=roomMicToggle()> enable</label></div>"
"<div id=roommicstat class=status style=margin-bottom:6px>...</div>"
"<div class=hint>Pulls the <b>other people's voices</b> from your Bigscreen room out of the cloud "
"(the room's mediasoup mic mix) and exposes them as a virtual microphone <b>BSDR_RoomMic</b> — record "
"the room into OBS, a call, etc. This is the room voice, separate from <b>BSDR_QuestMic</b> (your own "
"voice off the LAN). It also powers <b>computer control</b> when the <b>BSDR_QuestMic</b> isn&#8217;t available. "
"Requires <b>Internet sharing</b> on (that&#8217;s the cloud connection that carries the room). Linux "
"exposes a dedicated device; on Windows/macOS it routes into VB-CABLE/BlackHole.</div></div></div>"

"<div class=card><h2>Source</h2>"
"<div class=row>"
"<label><input type=radio name=src value=desktop onclick=pickSrc('desktop') style=width:auto> Desktop</label> "
"<label><input type=radio name=src value=file onclick=pickSrc('file') style=width:auto> Video file</label> "
"<label><input type=radio name=src value=webcam onclick=pickSrc('webcam') style=width:auto> Webcam</label> "
"<label><input type=radio name=src value=webcam3d onclick=pickSrc('webcam3d') style=width:auto> Stereo 3D (2 cams)</label></div>"
"<div class=row id=winrow><label style=width:auto;color:var(--muted)>Capture</label>"
"<select id=win class=grow onchange=selWin()></select>"
"<button onclick=loadWindows() title='Rescan windows/screens' style=width:auto>&#8635;</button></div>"
"<div class=hint id=winhint>Choose <b>Whole desktop</b>, a single <b>Screen:</b> (one monitor), or a "
"single <b>window</b>. On Wayland the portal shows its own picker instead.</div>"
/* Webcam picker: filled from /api/webcams (all four platforms enumerate — Linux V4L2, Windows
 * DirectShow, macOS AVFoundation, Android CameraManager). First select = camera (single) or LEFT eye
 * (stereo); second select = RIGHT eye, shown only in stereo mode. If enumeration ever returns nothing,
 * camCtl falls back to a manual device/index field (renderCams). */
"<div class=row id=camrow style=display:none><label style=width:auto;color:var(--muted)>Camera</label>"
"<span id=camsel class=grow></span>"
"<button onclick=loadCams() title='Rescan cameras' style=width:auto>&#8635;</button></div>"
"<div class=row id=camrowR style=display:none><label style=width:auto;color:var(--muted)>Right eye</label>"
"<span id=camselR class=grow></span></div>"
"<div class=hint id=camhint style=display:none>Stream a camera to the headset (and the cloud room). "
"Stereo 3D uses two cameras side-by-side as a real stereo pair — mount them ~6&#160;cm apart, level.</div>"
"<div class=row id=filerow><input id=path class=grow placeholder='/path/to/video.mp4, http(s)://… , rtsp://… , or playlist.txt' onchange=srcpath()>"
"<button onclick=openBrowse() style=width:auto>Browse&#8230;</button>"
"<button id=pause class=danger onclick=togglePause()>Stop</button></div>"
"<div class=hint>A video file, an <b>http/https/rtsp URL</b>, or a <b>.txt playlist</b> (one file "
"or URL per line, streamed in a loop). An in-VR media bar gives play/pause, seek, volume and exit; "
"the source's own audio streams too.</div>"
"<div class=row><label style=width:auto;color:var(--muted)>Video bitrate</label>"
"<input id=brate type=number min=0 step=1 style=width:90px placeholder=0 onchange=bitrateSet()>"
"<span class=hint style=margin-left:6px>Mbps — <b>0 = follow the headset</b>; any value overrides the "
"headset's bitrate (applied live). <span id=breff></span></span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>Encoder</label>"
"<select id=enc style=width:auto onchange=encoderSet()><option value=0>CPU — x264 (sharper text)</option>"
"<option value=1>GPU — NVENC (offload / high bitrate)</option></select>"
"<span class=hint style=margin-left:6px>x264 keeps low-bitrate text crisp; NVENC frees the CPU but "
"needs more bitrate for the same sharpness. Saved across restarts.</span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>iGPU (Linux)</label>"
"<label style=width:auto><input type=checkbox id=vaapi style=width:auto onchange=vaapiSet()> VAAPI encode</label>"
"<label style='width:auto;margin-left:12px'><input type=checkbox id=kmsgrab style=width:auto onchange=kmsgrabSet()> kmsgrab capture</label>"
"<span class=hint style=margin-left:6px>Linux + Intel/AMD iGPU: <b>VAAPI</b> encodes on the iGPU "
"(frees the dGPU). <b>kmsgrab</b> grabs the screen via DRM/KMS — zero-copy when paired with VAAPI, but "
"needs <code>CAP_SYS_ADMIN</code> (<code>setcap cap_sys_admin+ep</code>). Applied live (fresh keyframe); "
"falls back to the normal path if unavailable.</span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>Mode</label>"
"<select id=encmode style=width:auto onchange=encModeSet()><option value=0>Quality (default)</option>"
"<option value=1>Balanced (NVENC p6 / x264 faster)</option>"
"<option value=2>Performance (lighter CPU/GPU)</option></select>"
"<span class=hint style=margin-left:6px>Higher levels use a lighter preset (Quality = NVENC p7 + 2-pass; "
"Balanced = p6 single-pass / x264 faster; Performance = p4 single-pass / x264 superfast) — less CPU/GPU "
"for a small quality drop. Applies on the next stream start.</span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>x264 threads</label>"
"<input id=x264t type=number min=1 max=32 step=1 style=width:70px onchange=x264tSet()>"
"<span class=hint style=margin-left:6px>Software (CPU) encode only: use N frame threads to spread encode "
"across cores (still one NAL/frame). Adds ~(N-1) frames of latency, so leave at 1 unless a CPU host "
"can't keep up at high resolution. Applies on the next stream start.</span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>Max FPS</label>"
"<select id=fpscap style=width:auto onchange=fpsSet()><option value=0>Default (30)</option>"
"<option value=24>24</option><option value=20>20</option><option value=15>15</option></select>"
"<span class=hint style=margin-left:6px>Cap the capture/encode frame rate — the single biggest CPU "
"saver on a busy host. Applies on the next stream start.</span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>Wi-Fi</label>"
"<label style='width:auto;font-weight:400'><input id=lan1x type=checkbox style=width:auto onchange=lan1xSet()> "
"send video once (halve the LAN uplink)</label>"
"<span class=hint style=margin-left:6px>The desktop is normally sent twice for loss resilience; on a "
"congested Wi-Fi, once cuts airtime in half. Takes effect live.</span></div>"
"<div class=row><label style=width:auto;color:var(--muted)>&nbsp;</label>"
"<label style='width:auto;font-weight:400'><input id=wifiopt type=checkbox style=width:auto onchange=wifiOptSet()> "
"enable Wi-Fi network optimization</label>"
"<span class=hint style=margin-left:6px>DSCP/WMM-marks the video+audio packets so the Wi-Fi stack gives "
"them priority airtime over background traffic (downloads, other devices). Applies on the next stream "
"start.</span></div></div>"

"<div class=card><h2>2D&#8594;3D</h2>"
"<div class=hint>Convert the stream to side-by-side 3D in real time. <b>Fast</b> is a light "
"built-in depth heuristic (fine on old laptops); <b>AI</b> uses an external depth helper. Set your "
"Bigscreen screen to <b>SBS 3D</b> to see the effect. Uses the CPU encode path while on.</div>"
"<div class=row>"
"<label><input type=radio name=td value=0 onclick=threed() style=width:auto> Off</label> "
"<label><input type=radio name=td value=1 onclick=threed() style=width:auto> Fast</label> "
"<label><input type=radio name=td value=2 onclick=threed() style=width:auto> AI</label></div>"
"<div class=row><label style=width:120px;color:var(--muted)>AI model</label>"
"<select id=tdtier onchange=threed() class=grow>"
"<option value=0>External helper (command below)</option>"
"<option value=1>Built-in \342\200\242 CPU \342\200\224 Depth-Anything V2 Small (~99 MB)</option>"
"<option value=2>Built-in \342\200\242 small GPU \342\200\224 MiDaS DPT-Hybrid (~490 MB)</option>"
"<option value=3>Built-in \342\200\242 gaming desktop \342\200\224 MiDaS DPT-Large (~1.3 GB)</option>"
"<option value=-1 disabled>(auto)</option></select></div>"
/* External-helper command: shown only when the AI-model select is on "External helper". */
"<div id=tdext style=display:none>"
"<div class=row><input id=tdai class=grow placeholder='e.g. python3 scripts/bsdr-depth-helper.py --model midas_v21_small.onnx' onchange=threed()></div>"
"<div class=hint><b>AI mode needs this command set</b> — a ready-to-use helper ships in "
"<code>scripts/bsdr-depth-helper.py</code> (real depth with a MiDaS-small ONNX model, or a built-in "
"heuristic if you omit <code>--model</code>). It reads a small grayscale frame on stdin and writes "
"back a depth map (header <code>BSDD</code>+w+h, then w&#215;h bytes each way), at a reduced rate. "
"With no command, or if it stalls, 3D falls back to Fast. See <b>bsdr_agent</b>(1).</div></div>"
/* Built-in model manager: shown only when a built-in tier is selected. */
"<div id=tdbuiltin style=display:none>"
"<div class=row><button onclick=dlmodel() style=width:auto title='Download the selected built-in model now'>Download</button>"
"<span class=hint style=margin-left:8px>Built-in tiers run the depth model inside bsdrX. GPU tiers use the "
"platform accelerator (DirectML / CoreML / NNAPI / CUDA) and fall back to CPU. Pick a tier and "
"<b>Download</b> it (or import a zip below); until it lands, 3D uses the fast heuristic.</span></div>"
/* Live model manager: per-tier cached/absent state, in-flight download progress, cache location.
 * Filled by the status poll (renderModels) from status.models. */
"<div id=tdmodels class=hint style=margin-top:6px>models\342\200\246</div>"
"<div class=row><input id=tdzip class=grow placeholder='path to a model .zip on this machine'>"
"<button onclick=impmodel() style=width:auto>Import model zip</button></div></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Deepness</label>"
"<input id=tddeep type=range min=0 max=100 oninput='tddv.textContent=tddeep.value' onchange=threed() class=grow>"
"<span id=tddv style=width:2.5em;text-align:right>35</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Convergence</label>"
"<input id=tdconv type=range min=-50 max=50 oninput='tdcv.textContent=tdconv.value' onchange=threed() class=grow>"
"<span id=tdcv style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:auto><input id=tdfull type=checkbox style=width:auto onchange=threed()> Full resolution per eye (renders 3D at 2&#215; resolution, same screen shape; sharper, ~4&#215; encode cost)</label></div>"
"<div class=row><label style=width:auto><input id=tdswap type=checkbox style=width:auto onchange=threed()> Swap eyes (L/R)</label></div></div>"

"<div class=card><h2>Face swap <span class=badge>GPU</span></h2>"
"<div class=row><label style=width:auto><input id=fson type=checkbox style=width:auto onchange=faceswap()> "
"Swap faces in the stream to a source image (realtime deepfake)</label></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Source image</label>"
"<input id=fssrc class=grow placeholder='/path/to/face.jpg on this machine' onchange=faceswap()>"
"<button onclick=fsBrowse() style=width:auto>Browse&#8230;</button></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Compute</label>"
"<select id=fstier onchange=faceswap() class=grow>"
"<option value=1>CPU</option><option value=2 selected>GPU (CUDA/DirectML/CoreML/NNAPI)</option>"
"<option value=3>GPU (high)</option></select></div>"
"<div id=fsstat class=hint>face swap: off</div>"
"<div class=hint>Needs the insightface models (<code>det_10g.onnx</code>, <code>w600k_r50.onnx</code>, "
"<code>inswapper_128.onnx</code>) in the <b>faceswap</b> model dir. They're non-commercial so bsdrX "
"never bundles them — <b>Download models</b> fetches them from upstream, or drop them in / import a "
"zip yourself. Runs on the CPU encode path; use a GPU tier for realtime. Applies to whatever you're "
"streaming (desktop / webcam / file).</div>"
/* Model manager, mirrors 2D->3D: per-file present state + a one-click download. From status.faceswap.models. */
"<div class=row><button onclick=fsdl() style=width:auto title='Download the insightface models now'>Download models</button>"
"<span id=fsmodels class=hint style=margin-left:8px>models\342\200\246</span></div>"
"<div class=row><input id=fszip class=grow placeholder='path to a models .zip (buffalo_l.zip) on this machine'>"
"<button onclick=fsimp() style=width:auto>Import model zip</button></div></div>"

"<div class=card><h2>Voice assistant</h2>"
"<div class=hint>Configure speech-to-text and (for spoken desktop commands) an LLM here. "
"When <b>computer control</b> is on, a movable balloon appears over the desktop in VR — click "
"it to speak; bsdrX listens until you stop, transcribes, and the LLM runs the action.</div>"

"<div class=sub2><div class=t>Speech-to-text (STT)<span id=sttbadge class='badge free'>free online service</span></div>"
"<div class=row><input id=se class=grow placeholder='leave blank = free online service' onchange=voicecfg()></div>"
"<div class=row><input id=sm placeholder=whisper-1 style=width:150px onchange=voicecfg()>"
"<input id=st type=password placeholder=token style=width:180px onchange=voicecfg()></div>"
"<div class=hint id=stthint>Leave the URL blank to use a built-in <b>free online</b> "
"transcription service — no setup. For private/faster results, point it at your own "
"Whisper server (e.g. <code>http://localhost:8080/inference</code>) or any OpenAI-compatible "
"<code>/v1/audio/transcriptions</code> endpoint, plus its model and token.</div></div>"

"<div class=sub2><div class=t>Language model (LLM)<span id=llmbadge class='badge custom'>not set</span></div>"
"<div class=row><input id=le class=grow placeholder='https://api.openai.com/v1/chat/completions' onchange=voicecfg()></div>"
"<div class=row><input id=lm placeholder=gpt-4o-mini style=width:150px onchange=voicecfg()>"
"<input id=lt type=password placeholder=token style=width:180px onchange=voicecfg()></div>"
"<div class=hint>Required for spoken <i>commands</i> (the model decides which desktop "
"action to run). Any OpenAI-compatible chat endpoint works. Without it, speech is only typed out.</div></div>"

"<div class=row style=margin-top:12px><button class=p onclick=voicecfg()>Apply voice settings</button></div>"

"<div class=sub2><div class=t>Computer control<span id=ccbadge class='badge custom'>off</span></div>"
"<div id=ccstatus class=status style=margin-bottom:8px>...</div>"
"<div class=hint>Turns the in-VR voice-command <b>balloon</b> on. Drag it anywhere over the "
"desktop; a click starts a listen-until-silence capture, then the LLM drives the desktop "
"(type text, key combos, click, scroll, launch apps). Requires the <b>Quest mic</b> running "
"— the only source of your voice — and an LLM endpoint set above.</div>"
"<div class=row><label style=width:auto><input id=ccvis type=checkbox style=width:auto onchange=setVision()> "
"Vision (let the model take a desktop screenshot when a request needs it)</label></div>"
"<div class=row><label style=width:auto><input id=ownmiclocal type=checkbox style=width:auto onchange=ownMicLocalToggle()> "
"Use <b>this computer's microphone</b> as the Quest mic (no headset mic capture needed).</label></div>"
"<div class=row><label style=width:auto><input id=cloudmic type=checkbox style=width:auto onchange=cloudmicToggle()> "
"Quest mic via cloud room when the LAN sniffer/companion isn't available (e.g. Wi-Fi client isolation). Isolates the owner "
"(loudest speaker) only while a command is spoken.</label></div>"
"<div class=row><button id=ccbtn class=p onclick=toggleCompctl()>Enable computer control</button></div></div>"
"</div>"

/* Optional 3rd-party dependencies: shown only when this platform has any (empty on Linux-native/Android).
 * Each row: present ✓ / a How-to-install button linking to /deps/<id> (auto-install where automatable). */
"<div class=card id=depcard style=display:none><h2>Dependencies</h2>"
"<div class=hint>Optional external drivers/programs some features need. bsdrX bundles what its licence "
"allows; the rest link to the official installer with step-by-step instructions.</div>"
"<div id=deplist></div></div>"

/* server-side file-browser modal (fills the source path) */
"<div id=fb style=\"display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:99\" onclick=\"if(event.target==this)fbClose()\">"
"<div style=\"background:#1b1f2a;color:#e6e8ee;max-width:640px;margin:6vh auto;border-radius:10px;padding:14px;max-height:80vh;display:flex;flex-direction:column;border:1px solid #333\">"
"<div class=row style=align-items:center><b id=fbdir style=flex:1;overflow:hidden;text-overflow:ellipsis>/</b>"
"<button onclick=fbClose() style=width:auto>&#10005;</button></div>"
"<div id=fblist style=overflow:auto;margin-top:8px></div>"
"<div class=hint>Pick a video, or a .txt playlist.</div></div></div>"

/* donate modal: the three crypto addresses from bigscreen.nexlab.net */
"<div id=dn style=\"display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:99\" onclick=\"if(event.target==this)dnClose()\">"
"<div style=\"background:#1b1f2a;color:#e6e8ee;max-width:560px;margin:8vh auto;border-radius:10px;padding:16px;border:1px solid #333\">"
"<div class=row style=align-items:center><b style=flex:1>&#9829; Support development</b>"
"<button onclick=dnClose() style=width:auto>&#10005;</button></div>"
"<div class=hint>If bsdrX helps your Linux-in-VR setup, donations help fund headset testing, "
"reverse-engineering time, packaging and cross-platform builds. Thank you!</div>"
"<div class=sub2><div class=t>&#8383; Bitcoin (BTC)</div>"
"<div class=row><input class=grow readonly value='bc1ql5klyv78t0a5g59y3tczv8ejy9l740x3zg0cge' onclick=this.select()>"
"<button style=width:auto onclick=\"dnCopy(this,'bc1ql5klyv78t0a5g59y3tczv8ejy9l740x3zg0cge')\">Copy</button></div></div>"
"<div class=sub2><div class=t>&#926; Ethereum (ETH)</div>"
"<div class=row><input class=grow readonly value='0x3f707d3543A6C301B3Bf47eBc4B469e017a119B4' onclick=this.select()>"
"<button style=width:auto onclick=\"dnCopy(this,'0x3f707d3543A6C301B3Bf47eBc4B469e017a119B4')\">Copy</button></div></div>"
"<div class=sub2><div class=t>&#9678; Solana (SOL)</div>"
"<div class=row><input class=grow readonly value='G7iZQ3iQ7k5t9E9g8Y7u6i5t4r3e2w1q0P9O8I7U6Y5' onclick=this.select()>"
"<button style=width:auto onclick=\"dnCopy(this,'G7iZQ3iQ7k5t9E9g8Y7u6i5t4r3e2w1q0P9O8I7U6Y5')\">Copy</button></div></div>"
"<div class=hint>More at <a href='https://bigscreen.nexlab.net' target=_blank rel=noopener>bigscreen.nexlab.net</a>.</div>"
"</div></div>"

"<script>"
"async function api(p,b){return fetch(p,{method:b?'POST':'GET',body:b?JSON.stringify(b):null}).then(r=>r.json().catch(()=>({})))}"
"function login(){api('/api/login',{email:email.value,password:pw.value})}"
"function tlsToggle(){api('/api/tls',{insecure:tlsinsecure.checked?1:0})}"
"function logout(){api('/api/logout',{})}"
"function botLogin(){api('/api/bot/login',{email:botemail.value,password:botpw.value}).then(()=>{botpw.value=''})}"
"function botLogout(){api('/api/bot/logout',{})}"
"function botLeave(){botleavebtn.disabled=true;api('/api/bot/leave',{}).then(()=>{botleavebtn.disabled=false})}"
"function botStop(){api('/api/bot/stop',{})}"
"function botStart(){api('/api/bot/start',{})}"
"function botMode(){api('/api/bot/mode',{mode:botmode.value})}"
"function botFollowSet(){api('/api/bot/follow',{on:botfollow.checked?1:0})}"
"function botLoopSet(){api('/api/bot/loopback',{on:botloop.checked?1:0})}"
"function botSoloSet(){api('/api/bot/solo',{on:botsolo.checked?1:0})}"
"function botJoin(){botjoinbtn.disabled=true;botjoinbtn.textContent='Joining…';api('/api/bot/join',{}).then(r=>{botjoinbtn.disabled=false;botjoinbtn.textContent='Join my room';if(!r||!r.ok)alert('Bot room-join failed — check that your headset is in a room and the bot is logged in (see debug log).');})}"
"function sel(ip){api('/api/select',{ip:ip})}"
"let cams=[],camMode='desktop';"
"function srcRows(m){let cam=(m==='webcam'||m==='webcam3d');"
"winrow.style.display=(m==='desktop')?'flex':'none';winhint.style.display=(m==='desktop')?'block':'none';filerow.style.display=(m==='file')?'flex':'none';"
"camrow.style.display=cam?'flex':'none';camrowR.style.display=(m==='webcam3d')?'flex':'none';camhint.style.display=cam?'block':'none';}"
"function pickSrc(m){camMode=m;srcRows(m);"
"if(m==='webcam'||m==='webcam3d'){loadCams().then(pickCam);}else{api('/api/source',{mode:m,path:path.value});}}"
"async function loadCams(){let r=await api('/api/webcams');cams=(r&&r.cams)||[];renderCams();}"
"async function loadDeps(){let r=await api('/api/deps');let d=(r&&r.deps)||[];let card=document.getElementById('depcard');"
"if(!d.length){card.style.display='none';return;}card.style.display='';let h='';"
"d.forEach(function(x){let st=x.present?'<span class=\"badge free\">installed</span>':(x.bundled?'<span class=\"badge free\">bundled</span>':'<span class=\"badge custom\">not detected</span>');"
"let btn=x.present?'':(x.automatable?'<button style=width:auto onclick=\"depInstall(\\''+x.id+'\\')\">Install</button>':'<button style=width:auto onclick=\"window.open(\\'/deps/'+x.id+'\\',\\'_blank\\')\">How to install</button>');"
"h+='<div class=sub2><div class=t>'+x.name+' '+st+'</div><div class=hint>'+x.purpose+' &middot; <span style=opacity:.7>'+x.license+'</span></div>'+(btn?('<div class=row>'+btn+'</div>'):'')+'</div>';});"
"document.getElementById('deplist').innerHTML=h;}"
"async function depInstall(id){let r=await api('/api/deps/install',{id:id});if(r&&r.manual){window.open('/deps/'+id,'_blank');}else{await loadDeps();}}"
"function camCtl(id,cur){if(cams.length){return '<select id='+id+' class=grow onchange=pickCam()>'+cams.map(function(c){return '<option value=\"'+c.id+'\"'+(c.id===cur?' selected':'')+'>'+c.name+'</option>'}).join('')+'</select>';}"
"return '<input id='+id+' class=grow onchange=pickCam() value=\"'+(cur||'')+'\" placeholder=\"camera device or index\">';}"
"function renderCams(){camsel.innerHTML=camCtl('cam',window._cam||'');camselR.innerHTML=camCtl('camR',window._camR||'');}"
"function pickCam(){let a=document.getElementById('cam'),b=document.getElementById('camR');let d=a?a.value:'',d2=b?b.value:'';window._cam=d;window._camR=d2;api('/api/source',{mode:camMode,path:d,dev2:d2});}"
"function srcpath(){api('/api/source',{mode:'file',path:path.value})}"
"function blankToggle(){api('/api/blank',{on:blank.checked?1:0})}"
"function pointerModeToggle(){api('/api/pointermode',{touch:ptouch.checked?1:0})}"
"function bitrateSet(){api('/api/bitrate',{mbps:+brate.value||0})}"
"function encoderSet(){api('/api/encoder',{gpu:+enc.value})}"
"function vaapiSet(){api('/api/vaapi',{on:vaapi.checked?1:0})}"
"function kmsgrabSet(){api('/api/kmsgrab',{on:kmsgrab.checked?1:0})}"
"function encModeSet(){api('/api/encmode',{level:+encmode.value})}"
"function x264tSet(){api('/api/x264threads',{n:+x264t.value})}"
"function fpsSet(){api('/api/fpscap',{fps:+fpscap.value})}"
"function lan1xSet(){api('/api/lan1x',{on:lan1x.checked?1:0})}"
"function wifiOptSet(){api('/api/wifiopt',{on:wifiopt.checked?1:0})}"
"function fbClose(){document.getElementById('fb').style.display='none'}"
"function showDonate(){document.getElementById('dn').style.display='block'}"
"function dnClose(){document.getElementById('dn').style.display='none'}"
"function dnCopy(b,a){let d=()=>{b.textContent='Copied';setTimeout(()=>b.textContent='Copy',1200)};"
"if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(a).then(d).catch(()=>{});}else{d();}}"
"function fbJoin(d,n){return d==='/'?'/'+n:d+'/'+n}"
"let fbTarget='file';"
"function openBrowse(){fbTarget='file';let p=path.value||'';let d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';fbBrowse(d)}"
"function fsBrowse(){fbTarget='fs';let p=fssrc.value||'';let d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';fbBrowse(d)}"
"async function fbBrowse(d){let r=await api('/api/browse?dir='+encodeURIComponent(d));if(!r||r.dir==null)return;"
"document.getElementById('fb').style.display='block';document.getElementById('fbdir').textContent=r.dir;"
"let L=document.getElementById('fblist');L.innerHTML='';"
"let mk=(t,fn)=>{let b=document.createElement('div');b.textContent=t;b.style.cssText='padding:7px 9px;cursor:pointer;border-radius:6px';b.onmouseover=()=>b.style.background='rgba(255,255,255,.08)';b.onmouseout=()=>b.style.background='';b.onclick=fn;L.appendChild(b)};"
"if(r.parent!=null&&r.parent!==r.dir)mk('\\uD83D\\uDCC1 ..',()=>fbBrowse(r.parent));"
"(r.entries||[]).sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).forEach(e=>{"
"if(e.dir)mk('\\uD83D\\uDCC1 '+e.name,()=>fbBrowse(fbJoin(r.dir,e.name)));"
"else mk('\\uD83C\\uDFAC '+e.name,()=>fbPick(fbJoin(r.dir,e.name)))})}"
"function fbPick(f){if(fbTarget==='fs'){fssrc.value=f;faceswap();}else if(fbTarget&&document.getElementById(fbTarget)){document.getElementById(fbTarget).value=f;}else{path.value=f;srcpath();}fbClose()}"
"async function loadWindows(){let ws=await api('/api/windows');let e=document.getElementById('win');e.innerHTML='';(ws||[]).forEach((o,i)=>{let p=document.createElement('option');p.value=i;p.textContent=o.title+(o.w?(' ('+o.w+'x'+o.h+')'):'');p.dataset.geo=JSON.stringify(o);e.appendChild(p)})}"
"function selWin(){let e=document.getElementById('win');let o=JSON.parse(e.options[e.selectedIndex].dataset.geo);api('/api/region',{x:o.x,y:o.y,w:o.w,h:o.h})}"
"function togglePause(){api('/api/pause',{toggle:true})}"
"function disconnect(){api('/api/disconnect',{})}"
"function toggleShare(){api('/api/share',{toggle:true})}"
#if defined(__ANDROID__)
"function toggleSniff(){let on=snbtn.dataset.on==='1';api('/api/sniff',{want:on?0:1,password:''})}"
"function snMethodChange(){relayrow.style.display=(+snmethod.value===2)?'flex':'none';api('/api/sniffmethod',{method:+snmethod.value})}"
#else
/* Non-blocking Wi-Fi guardrail: ARP-MITM over Wi-Fi is unreliable and can briefly drop the headset's
 * LAN link. When the operator picks/starts MITM on a Wi-Fi NIC, ask — OK keeps trying MITM, Cancel lets
 * them change strategy (pick Relay). Only prompts when the server reports the capture NIC is wireless. */
"function mitmWifiOk(){return confirm('MITM over Wi-Fi is unreliable and may briefly drop the headset connection (the session can time out).\\n\\nOK = try MITM anyway\\nCancel = keep the current method (use Relay / router companion instead)')}"
"function toggleSniff(){let on=snbtn.dataset.on==='1';if(!on&&+snmethod.value===1&&snmethod.dataset.wifi==='1'&&!mitmWifiOk())return;api('/api/sniff',{want:on?0:1,password:snpw.value}).then(()=>{snpw.value=''})}"
"function snMethodChange(){let m=+snmethod.value;if(m===1&&snmethod.dataset.wifi==='1'&&!mitmWifiOk()){snmethod.value=snmethod.dataset.cur||0;relayrow.style.display=(+snmethod.value===2)?'flex':'none';return;}snmethod.dataset.cur=m;relayrow.style.display=(m===2)?'flex':'none';api('/api/sniffmethod',{method:m})}"
#endif
"function voicefx(){api('/api/voicefx',{on:vfxon.checked?1:0,gender:+vgender.value,formant:+vformant.value,volume:+vvolume.value,robot:+vrobot.value,echo:+vecho.value,whisper:+vwhisper.value,substitute:vsub.checked?1:0})}"
"var VPRESETS={feminize:{g:45,fm:30,vo:0,r:0,e:0,w:0},masculinize:{g:-45,fm:-25,vo:0,r:0,e:0,w:0},"
"younger:{g:28,fm:18,vo:0,r:0,e:0,w:0},older:{g:-22,fm:-15,vo:0,r:0,e:6,w:18},"
"chipmunk:{g:85,fm:40,vo:0,r:0,e:0,w:0},deep:{g:-70,fm:-30,vo:5,r:0,e:0,w:0},"
"robot:{g:-10,fm:0,vo:0,r:75,e:10,w:0},reset:{g:0,fm:0,vo:0,r:0,e:0,w:0}};"
"function vpreset(n){var p=VPRESETS[n];if(!p)return;vfxon.checked=true;"
"vgender.value=p.g;vgv.textContent=p.g;vformant.value=p.fm;vfmv.textContent=p.fm;"
"vvolume.value=p.vo;vvv.textContent=p.vo;vrobot.value=p.r;vrv.textContent=p.r;"
"vecho.value=p.e;vev.textContent=p.e;vwhisper.value=p.w;vwv.textContent=p.w;voicefx()}"
/* AI voice (RVC) */
"function voiceai(){api('/api/voiceai',{on:vaion.checked?1:0,tier:+vaitier.value,voice:vaivoice.value||'',key:+vaikey.value})}"
"function vaidl(){api('/api/voice-basedl',{})}"
"function vaiimp(){if(vaizip.value)api('/api/voice-import',{path:vaizip.value})}"
"function vaiAddUrl(){if(vaddurl.value&&vaddid.value)api('/api/voice-add-url',{url:vaddurl.value,id:vaddid.value,name:vaddname.value||''}).then(()=>{vaddurl.value=''})}"
"function vaiAddFile(){if(vaddpath.value&&vaddid.value)api('/api/voice-add-file',{path:vaddpath.value,id:vaddid.value,name:vaddname.value||''}).then(()=>{vaddpath.value=''})}"
"function vaddBrowse(){fbTarget='vaddpath';let p=vaddpath.value||'';let d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';fbBrowse(d)}"
"function vaiDel(id){if(confirm('Delete voice '+id+'?'))api('/api/voice-delete',{id:id})}"
"function vpSave(s){let nm=prompt('Preset name');if(nm)api('/api/voice-preset',{action:'save',slot:s,name:nm})}"
"function vpApply(s){api('/api/voice-preset',{action:'apply',slot:s})}"
"function vpDel(s){api('/api/voice-preset',{action:'delete',slot:s})}"
"function renderVoiceAI(v){if(!v)return;if(document.activeElement!==vaion)vaion.checked=!!v.on;"
"if(document.activeElement!==vaitier)vaitier.value=v.tier;if(document.activeElement!==vaikey){vaikey.value=v.key;vaikv.textContent=v.key;}"
"let sel=vaivoice,cur=sel.value;sel.innerHTML='<option value=\"\">(pick a voice)</option>'+(v.voices||[]).map(o=>'<option value=\"'+o.id+'\">'+o.name+'</option>').join('');"
"if(v.voice)sel.value=v.voice;else if(cur)sel.value=cur;"
"let st=v.status||'';if(!v.available)st='ONNX not built in this agent';else if(!v.baseReady)st='engine base models needed';vaistat.textContent='Engine: '+st;"
"let d=v.dl||{};vaidls.textContent=d.active?(d.name+' '+(d.pct<0?'…':d.pct+'%')):(d.err?('error: '+d.err):(v.baseReady?'base models present':''));"
"vailist.innerHTML=(v.voices||[]).map(o=>'<div class=row style=gap:6px><span class=grow>'+o.name+' <span style=opacity:.6>('+o.id+', '+o.sr+'Hz)</span></span><button onclick=\"vaiDel(\\''+o.id+'\\')\">del</button></div>').join('')||'<i>no voices yet — add one above</i>';"
"vaipresets.innerHTML=(v.presets||[]).map((p,i)=>'<div class=row style=gap:6px><span class=grow>'+(p.name?('#'+(i+1)+' '+p.name+(p.ai?' <span style=opacity:.6>AI</span>':'')):('<i>slot '+(i+1)+' empty</i>'))+'</span>'+(p.name?'<button onclick=vpApply('+i+')>apply</button><button onclick=vpDel('+i+')>del</button>':'')+'<button onclick=vpSave('+i+')>save</button></div>').join('');}"
"function relayPortSet(){api('/api/relayport',{port:+relayport.value})}"
"function faceswap(){api('/api/faceswap',{on:fson.checked?1:0,tier:+fstier.value,source:fssrc.value})}"
"function voicecfg(){api('/api/voice',{stt:se.value,sttModel:sm.value,sttToken:st.value,llm:le.value,llmModel:lm.value,llmToken:lt.value})}"
"function toggleCompctl(){let on=ccbtn.dataset.on==='1';api('/api/compctl',{enable:on?0:1,vision:ccvis.checked?1:0})}"
"function setVision(){let on=ccbtn.dataset.on==='1';api('/api/compctl',{enable:on?1:0,vision:ccvis.checked?1:0})}"
"function cloudmicToggle(){api('/api/cloudmic',{on:cloudmic.checked?1:0})}"
"function roomMicToggle(){api('/api/roommic',{on:roommic.checked?1:0})}"
"function ownMicLocalToggle(){api('/api/ownmiclocal',{on:ownmiclocal.checked?1:0})}"
"function tdTierUI(){let ext=(+tdtier.value===0);tdext.style.display=ext?'block':'none';tdbuiltin.style.display=ext?'none':'block';}"
"function threed(){tdTierUI();let m=0;for(const r of document.getElementsByName('td'))if(r.checked)m=+r.value;api('/api/threed',{mode:m,deepness:+tddeep.value,convergence:+tdconv.value,swap:tdswap.checked?1:0,full:tdfull.checked?1:0,tier:+tdtier.value,ai:tdai.value})}"
"function impmodel(){if(!tdzip.value)return;api('/api/model-import',{path:tdzip.value}).then(r=>alert(r&&r.imported>=0?('imported '+r.imported+' model(s)'):'import failed'))}"
"function dlmodel(){let t=+tdtier.value;if(t<1){alert('Pick a built-in tier (CPU / GPU / desktop) to download.');return;}api('/api/model-download',{tier:t}).then(r=>{if(!r||r.ok!==true)alert('cannot start download'+(r&&r.err?': '+r.err:''))})}"
"function fsdl(){api('/api/faceswap-download',{}).then(r=>{if(!r||r.ok!==true)alert('cannot start download')})}"
"function fsimp(){if(!fszip.value)return;api('/api/faceswap-import',{path:fszip.value}).then(r=>alert(r&&r.imported>=0?('imported '+r.imported+' model(s)'):'import failed'))}"
"function renderFsModels(m){if(!m){fsmodels.textContent='';return;}"
"let d=m.dl,rows=(m.files||[]).map(function(f){return f.name+': '+(f.present?'\\u2713':'\\u2717');}).join('  ');"
"if(d&&d.active)rows+='  \\u2193 '+(d.name||'')+' '+(d.pct>=0?d.pct+'%':((d.done/1048576).toFixed(0)+' MB'));"
"else if(d&&d.err)rows+='  <span style=color:#f85149>'+d.err+'</span>';"
"else if(m.ready)rows='<span style=color:#3fb950>\\u2713 all models present</span>';"
"fsmodels.innerHTML=rows+'  <span style=color:var(--muted)>('+(m.dir||'?')+')</span>';}"
"function renderModels(m){if(!m){tdmodels.textContent='';return;}"
"let d=m.dl,rows=(m.tiers||[]).map(function(x){"
"let st;if(x.present)st='<span style=color:#3fb950>\\u2713 cached</span>';"
"else if(d&&d.active&&d.tier===x.tier)st='<span style=color:#d29922>\\u2193 downloading '+(d.pct>=0?d.pct+'%':((d.done/1048576).toFixed(0)+' MB'))+'</span>';"
"else st='<span style=color:var(--muted)>not downloaded (~'+x.mb+' MB)</span>';"
"return '<div style=\"display:flex;gap:8px;align-items:center\"><span style=flex:1>'+x.name+'</span>'+st+'</div>';}).join('');"
"let err=(d&&d.err)?('<div style=color:#f85149>download error: '+d.err+'</div>'):'';"
"tdmodels.innerHTML=rows+err+'<div style=color:var(--muted);margin-top:4px>cache: '+(m.dir||'?')+'</div>';}"
"function dot(b){return '<span class=\"dot '+(b?'on':'off')+'\"></span>'}"
/* collapsible panels: every card except the first becomes a click-to-toggle panel whose open/closed
 * state persists in localStorage, with an on/off badge in the header that stays visible when collapsed */
"function cardKey(t){t=t.toLowerCase();"
"if(t.indexOf('second account')>=0)return'bot';"
"if(t.indexOf('quest')>=0&&t.indexOf('mic')<0)return'quest';"
"if(t.indexOf('headset mic')>=0)return'mic';"
"if(t.indexOf('source')>=0)return'source';"
"if(t.indexOf('2d')>=0)return'td';"
"if(t.indexOf('face swap')>=0)return'fs';"
"if(t.indexOf('voice assistant')>=0)return'va';"
"if(t.indexOf('dependencies')>=0)return'dep';"
"return t.replace(/[^a-z0-9]/g,'').slice(0,12);}"
"function setupPanels(){document.querySelectorAll('.card').forEach(function(c,i){"
"if(i===0)return;var h=c.querySelector('h2');if(!h||h.dataset.clp)return;h.dataset.clp='1';"
"var key=cardKey(h.textContent);h.classList.add('clp');"
"var r=document.createElement('span');r.className='hdrr';"
"var en=document.createElement('span');en.className='en';en.id='en_'+key;"
"var ch=document.createElement('span');ch.className='chev';ch.textContent='\\u25be';"
"r.appendChild(en);r.appendChild(ch);h.appendChild(r);"
"if(localStorage.getItem('clp:'+key)==='1')c.classList.add('col');"
"h.addEventListener('click',function(){c.classList.toggle('col');"
"localStorage.setItem('clp:'+key,c.classList.contains('col')?'1':'0');});});}"
"function setEn(k,on,txt){var e=document.getElementById('en_'+k);if(!e)return;"
"if(on===null){e.className='en';e.textContent=txt||'';}else{e.className='en '+(on?'y':'n');e.textContent=txt||(on?'on':'off');}}"
"async function tick(){let s=await api('/api/status');"
"cloud.innerHTML=dot(s.cloud.loggedIn)+'<span>'+(s.cloud.loggedIn?('Connected as '+s.cloud.name):('Not connected — '+s.cloud.msg))+'</span>'+(s.cloud.loggedIn?'<button onclick=logout() style=margin-left:auto>Log out</button>':'');"
"loginform.style.display=s.cloud.loggedIn?'none':'flex';"
"loginform2.style.display=s.cloud.loggedIn?'none':'flex';"
"if(s.bot){let b=s.bot,bi=b.loggedIn;"
"bot.innerHTML=dot(bi)+'<span>'+(bi?('Bot: '+b.name+(b.joined?(' — in room ('+(b.room||'')+')'):'')+(b.msg?(' — '+b.msg):'')):('Not logged in'+(b.msg?(' — '+b.msg):'')))+'</span>';"
"botlogin.style.display=(!bi&&!b.stopped)?'flex':'none';"
"botstart.style.display=(!bi&&b.stopped)?'flex':'none';"
"botactions.style.display=bi?'flex':'none';"
"botfollowrow.style.display=bi?'flex':'none';"
"botlooprow.style.display=bi?'flex':'none';"
"if(document.getElementById('botfollow')&&document.activeElement!==botfollow)botfollow.checked=!!b.follow;"
"if(document.getElementById('botloop')&&document.activeElement!==botloop)botloop.checked=!!b.loopback;"
"if(document.getElementById('botsolo')&&document.activeElement!==botsolo)botsolo.checked=!!b.solo;"
"if(b.mode&&document.getElementById('botmode')&&document.activeElement!==botmode)botmode.value=b.mode;"
"if(document.getElementById('botleavebtn'))botleavebtn.style.display=b.joined?'':'none';"
"if(document.getElementById('botjoinbtn')){botjoinbtn.textContent=b.joined?'Re-join my room':'Join my room';}}"
"if(document.activeElement!==tlsinsecure)tlsinsecure.checked=!!s.tlsInsecure;"
"cloudshare.style.display=s.cloud.loggedIn?'flex':'none';"
"if(s.cloud.loggedIn){let sh=s.cloud.internetSharing;sharelbl.textContent='Internet sharing: '+(sh?'ON':'off');sharebtn.textContent=sh?'Stop sharing':'Share to Internet';sharebtn.className=sh?'danger':'p';}"
"quest.innerHTML=dot(s.quest.paired)+'<span>'+(s.quest.paired?('Connected: '+s.quest.name+' ('+s.quest.ip+')'):'No headset connected')+'</span>'+(s.quest.streaming?'<span class=pill>streaming</span>':'')+(s.quest.paired?'<button class=danger style=margin-left:auto onclick=disconnect()>Disconnect</button>':'');"
"if(s.sniff){let sn=s.sniff;sniff.innerHTML=dot(sn.active)+'<span>'+(sn.active?('On — '+(sn.msg||'active')):('Off'+(sn.msg?(' — '+sn.msg):'')))+'</span>';snbtn.dataset.on=sn.want?'1':'0';snbtn.textContent=sn.want?'Stop mic':'Start mic';snbtn.className=sn.want?'danger':'p';"
"if(document.activeElement!==snmethod&&sn.method!==undefined){snmethod.value=sn.method;snmethod.dataset.cur=sn.method;}"
"if(sn.wifi!==undefined)snmethod.dataset.wifi=sn.wifi?'1':'0';"
"relayrow.style.display=(sn.method===2)?'flex':'none';"
"if(document.activeElement!==relayport&&sn.relayPort!==undefined)relayport.value=sn.relayPort||'';"
"if(document.activeElement!==vfxon&&sn.fxOn!==undefined)vfxon.checked=sn.fxOn;"
"if(document.activeElement!==vgender&&sn.gender!==undefined){vgender.value=sn.gender;vgv.textContent=sn.gender;}"
"if(document.activeElement!==vformant&&sn.formant!==undefined){vformant.value=sn.formant;vfmv.textContent=sn.formant;}"
"if(document.activeElement!==vvolume&&sn.volume!==undefined){vvolume.value=sn.volume;vvv.textContent=sn.volume;}"
"if(document.activeElement!==vrobot&&sn.robot!==undefined){vrobot.value=sn.robot;vrv.textContent=sn.robot;}"
"if(document.activeElement!==vecho&&sn.echo!==undefined){vecho.value=sn.echo;vev.textContent=sn.echo;}"
"if(document.activeElement!==vwhisper&&sn.whisper!==undefined){vwhisper.value=sn.whisper;vwv.textContent=sn.whisper;}"
"if(document.activeElement!==vsub&&sn.substitute!==undefined)vsub.checked=sn.substitute;}"
"let h='';for(const q of s.quests){let on=s.selected===q.ip;h+=\"<div class=q><span class=nm>\"+(on?'\\u2713 ':'')+q.name+' <span style=color:var(--muted)>'+q.ip+\"</span></span><button onclick=\\\"sel('\"+q.ip+\"')\\\">\"+(on?'Selected':'Use')+'</button></div>'}"
"if(s.quests.length>1||s.selected)h='<div class=hint style=margin-top:8px>Choose a headset:</div>'+h;quests.innerHTML=h;"
"for(const r of document.getElementsByName('src'))r.checked=(r.value===s.source.mode);"
"if(document.activeElement!==path)path.value=(s.source.mode==='file'?(s.source.path||''):path.value);"
/* reflect the persisted source: show the right rows, and populate the camera picker once */
"{let m=s.source.mode;camMode=m;srcRows(m);"
"if((m==='webcam'||m==='webcam3d')&&!camsel.firstChild){window._cam=s.source.path;window._camR=s.source.path2;loadCams();}}"
"if(document.activeElement!==blank)blank.checked=!!s.blank;"
"if(document.activeElement!==ptouch)ptouch.checked=!!s.pointerTouch;"
"if(s.quality){if(document.activeElement!==brate)brate.value=s.quality.brOverride?(s.quality.brOverride/1e6):'';"
"breff.textContent='(now '+((s.quality.bitrate||0)/1e6).toFixed(1)+' Mbps'+(s.quality.brOverride?', overriding':', from headset')+')';"
"if(document.activeElement!==enc)enc.value=s.quality.gpuEncode?'1':'0';"
"if(document.activeElement!==vaapi&&s.quality.vaapi!==undefined)vaapi.checked=!!s.quality.vaapi;"
"if(document.activeElement!==kmsgrab&&s.quality.kmsgrab!==undefined)kmsgrab.checked=!!s.quality.kmsgrab;"
"if(document.activeElement!==encmode&&s.quality.encLevel!==undefined)encmode.value=''+s.quality.encLevel;"
"if(document.activeElement!==x264t&&s.quality.x264Threads!==undefined)x264t.value=''+(s.quality.x264Threads||1);"
"if(document.activeElement!==fpscap&&s.quality.fpsCap!==undefined)fpscap.value=s.quality.fpsCap;"
"if(document.activeElement!==lan1x&&s.quality.lan1x!==undefined)lan1x.checked=s.quality.lan1x;"
"if(document.activeElement!==wifiopt&&s.quality.wifiOpt!==undefined)wifiopt.checked=s.quality.wifiOpt;}"
"blankrow.style.display=s.android?'none':'';"   /* screen-blank is desktop-only */
"if(document.activeElement!==cloudmic)cloudmic.checked=!!s.cloudMic;"
"if(document.activeElement!==ownmiclocal)ownmiclocal.checked=!!s.ownerMicLocal;"
"if(document.activeElement!==roommic)roommic.checked=!!s.roomMic;"
"{let sh=s.cloud&&s.cloud.internetSharing;roommicstat.innerHTML=dot(!!s.roomMic&&sh)+'<span>'+(!s.roomMic?'Off':(sh?'On — consuming the room voice (BSDR_RoomMic)':'Waiting — turn on Internet sharing to reach the room'))+'</span>';}"
"if(s.threed){let t=s.threed;for(const r of document.getElementsByName('td'))r.checked=(+r.value===t.mode);"
"if(document.activeElement!==tddeep){tddeep.value=t.deepness;tddv.textContent=t.deepness;}"
"if(document.activeElement!==tdconv){tdconv.value=t.convergence;tdcv.textContent=t.convergence;}"
"if(document.activeElement!==tdswap)tdswap.checked=!!t.swap;"
"if(document.activeElement!==tdfull)tdfull.checked=t.full!==false;"
"if(document.activeElement!==tdtier&&t.tier!==undefined)tdtier.value=t.tier;"
"if(document.activeElement!==tdai)tdai.value=t.ai||'';tdTierUI();}"
"renderModels(s.models);"
"if(s.faceswap){let fx=s.faceswap;if(document.activeElement!==fson)fson.checked=fx.on;"
"if(document.activeElement!==fssrc)fssrc.value=fx.source||'';"
"if(document.activeElement!==fstier&&fx.tier)fstier.value=fx.tier;"
"fsstat.textContent='face swap: '+(fx.status||(fx.on?'on':'off'));renderFsModels(fx.models);}"
"if(s.voiceai)renderVoiceAI(s.voiceai);"
"if(document.activeElement!==se)se.value=s.voice.stt||'';"
"if(document.activeElement!==sm)sm.value=s.voice.sttModel||'';"
"if(document.activeElement!==le)le.value=s.voice.llm||'';"
"if(document.activeElement!==lm)lm.value=s.voice.llmModel||'';"
"st.placeholder=s.voice.sttToken?'(set)':'token';lt.placeholder=s.voice.llmToken?'(set)':'token';"
"let sf=!s.voice.stt;sttbadge.textContent=sf?'free online service':'custom endpoint';sttbadge.className='badge '+(sf?'free':'custom');"
"let lf=!!s.voice.llm;llmbadge.textContent=lf?'configured':'not set';llmbadge.className='badge '+(lf?'free':'custom');"
"if(s.compctl){let cc=s.compctl,ready=lf&&(s.android||(s.sniff&&s.sniff.active));"
"ccbtn.dataset.on=cc.want?'1':'0';ccbtn.textContent=cc.want?'Disable computer control':'Enable computer control';ccbtn.className=cc.want?'danger':'p';"
"ccbtn.disabled=!ready&&!cc.want;"
"ccstatus.innerHTML=dot(cc.active)+'<span>'+(cc.active?('Armed — '+(cc.msg||'balloon active')):(cc.want?('Pending — '+(cc.msg||'waiting')):(ready?'Off — ready to enable':(s.android?'Off — set an LLM first':'Off — turn on the Quest mic and set an LLM first'))))+'</span>';"
"ccbadge.textContent=cc.active?'armed':(cc.want?'pending':'off');ccbadge.className='badge '+(cc.active?'free':'custom');"
"if(document.activeElement!==ccvis)ccvis.checked=!!cc.vision;}"
"let pb=document.getElementById('pause');pb.textContent=s.quest.paused?'Restart':'Stop';pb.className=s.quest.paused?'p':'danger';"
/* collapsed-panel enabled/disabled badges (computed from the same status) */
"setEn('bot',!!(s.bot&&s.bot.loggedIn),s.bot&&s.bot.loggedIn?(s.bot.joined?'in room':'on'):'off');"
"setEn('quest',!!s.quest.paired,s.quest.paired?(s.quest.streaming?'streaming':'connected'):'off');"
"setEn('mic',!!((s.sniff&&(s.sniff.active||s.sniff.fxOn))||s.roomMic||(s.voiceai&&s.voiceai.on)),"
"(s.sniff&&s.sniff.active)?'mic on':(((s.sniff&&s.sniff.fxOn)||(s.voiceai&&s.voiceai.on))?'voice fx':(s.roomMic?'room mic':'off')));"
"setEn('source',null,(s.source&&s.source.mode)?s.source.mode:'');"
"setEn('td',!!(s.threed&&s.threed.mode>0),(s.threed&&s.threed.mode>0)?'3D on':'off');"
"setEn('fs',!!(s.faceswap&&s.faceswap.on),(s.faceswap&&s.faceswap.on)?'on':'off');"
"setEn('va',!!(s.compctl&&(s.compctl.active||s.compctl.want)),s.compctl?(s.compctl.active?'armed':(s.compctl.want?'pending':'off')):'off');"
"}"
"setupPanels();setInterval(tick,1000);tick();loadWindows();loadDeps();"
"</script></body></html>";

static void respond(bsdr_socket_t c, int code, const char *ctype, const char *body, size_t blen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"   /* never cache the panel/API — a stale page kept old JS around */
        "Connection: close\r\n\r\n", code, ctype, blen);
    if (n < 0) return;
    size_t hn = (size_t)n < sizeof(hdr) ? (size_t)n : sizeof(hdr) - 1;  /* clamp truncation */
    bsdr_send_all(c, hdr, hn);
    if (blen) bsdr_send_all(c, body, blen);
}

static void handle(struct bsdr_webui *w, bsdr_socket_t c, const char *method,
                   const char *path, const char *body) {
    bsdr_app *a = w->app;
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        respond(c, 200, "text/html", PAGE, sizeof(PAGE) - 1);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/deps/", 6) == 0) {
        /* Per-dependency install instructions page (opened in a new tab from the Dependencies card). */
        const char *frag = bsdr_dep_page(path + 6);
        if (!frag) { respond(c, 404, "text/html", "<h1>Unknown dependency</h1>", 27); }
        else {
            static char pg[8192];
            int n = snprintf(pg, sizeof pg,
                "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
                "<title>bsdrX — install</title><style>body{font:15px/1.6 system-ui,sans-serif;max-width:44rem;"
                "margin:2rem auto;padding:0 1rem;color:#222;background:#fafafa}h1{font-size:1.4rem}code{background:#eee;"
                "padding:.1em .3em;border-radius:3px}a{color:#1769c0}ol,ul{padding-left:1.3rem}"
                "@media(prefers-color-scheme:dark){body{background:#181a1b;color:#ddd}code{background:#333}a{color:#6ab0ff}}"
                "</style>%s<p style=margin-top:2rem><a href=\"/\">&larr; back to bsdrX</a></p>", frag);
            respond(c, 200, "text/html", pg, n > 0 ? (size_t)n : 0);
        }
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/deps") == 0) {
        /* List this platform's optional 3rd-party deps + whether each is present. */
        bsdr_dep deps[16];
        int nd = bsdr_deps_list(deps, 16);
        char out[4096]; int ol = snprintf(out, sizeof out, "{\"deps\":[");
        for (int i = 0; i < nd && ol < (int)sizeof(out) - 512; i++) {
            char en[128], ep[256], el[128];
            bsdr_json_escape(en, sizeof en, deps[i].name);
            bsdr_json_escape(ep, sizeof ep, deps[i].purpose);
            bsdr_json_escape(el, sizeof el, deps[i].license);
            ol += snprintf(out + ol, sizeof(out) - ol,
                "%s{\"id\":\"%s\",\"name\":\"%s\",\"purpose\":\"%s\",\"license\":\"%s\",\"present\":%s,\"bundled\":%s,\"automatable\":%s}",
                i ? "," : "", deps[i].id, en, ep, el,
                deps[i].present ? "true" : "false", deps[i].bundled ? "true" : "false",
                deps[i].automatable ? "true" : "false");
        }
        ol += snprintf(out + ol, sizeof(out) - ol, "]}");
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/deps/install") == 0) {
        char id[64] = ""; bsdr_json_get_str(body, "id", id, sizeof id);
        char msg[256] = ""; int r = bsdr_dep_install(id, msg, sizeof msg);
        char em[512]; bsdr_json_escape(em, sizeof em, msg);
        char out[700]; int ol = snprintf(out, sizeof out,
            "{\"ok\":%s,\"manual\":%s,\"msg\":\"%s\"}",
            r == 1 ? "true" : "false", r == 0 ? "true" : "false", em);
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        static char json[20480];   /* room for the faceswap + voiceai (voice library + presets) objects */
        size_t n = bsdr_app_status_json(a, json, sizeof(json));
        respond(c, 200, "application/json", json, n);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/login") == 0) {
        char email[128] = "", pw[128] = "";
        bsdr_json_get_str(body, "email", email, sizeof(email));
        bsdr_json_get_str(body, "password", pw, sizeof(pw));
        bsdr_app_login(a, email, pw);                 /* blocking HTTPS */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/logout") == 0) {
        bsdr_app_logout(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/login") == 0) {
        char email[128] = "", pw[128] = "";
        bsdr_json_get_str(body, "email", email, sizeof(email));
        bsdr_json_get_str(body, "password", pw, sizeof(pw));
        bsdr_app_bot_login(a, email, pw);             /* blocking HTTPS (second account) */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/logout") == 0) {
        bsdr_app_bot_logout(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/join") == 0) {
        bool ok = bsdr_app_bot_join_room(a);          /* join the host's current room as the bot */
        respond(c, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/mode") == 0) {
        char mode[8] = ""; bsdr_json_get_str(body, "mode", mode, sizeof mode);
        bsdr_app_bot_set_mode(a, mode);               /* "audio" | "full"; persisted */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/follow") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_bot_follow(a, on != 0);          /* follow the operator between rooms; persisted */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/loopback") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_bot_loopback(a, on != 0);        /* bot room audio -> BSDR_RoomMic; persisted */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/solo") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_bot_solo_owner(a, on != 0);      /* "listen only to me"; persisted, live */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/leave") == 0) {
        bool ok = bsdr_app_bot_leave_room(a);         /* leave the room, stay logged in */
        respond(c, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/stop") == 0) {
        bsdr_app_bot_stop(a);                         /* disconnect but remember the login */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/start") == 0) {
        bsdr_app_bot_start(a);                         /* reconnect from the remembered session */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/tls") == 0) {
        double insecure = 0; bsdr_json_get_double(body, "insecure", &insecure);
        bsdr_tls_set_insecure(insecure != 0);   /* default verifies; toggle for a broken CA store */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/blank") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_blank(a, on != 0);   /* takes effect while the Quest is connected */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/pointermode") == 0) {
        double touch = 0; bsdr_json_get_double(body, "touch", &touch);
        bsdr_app_set_pointer_touch(a, touch != 0);   /* mouse vs real touch (live, persisted) */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cloudmic") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_cloud_mic_fallback(a, on != 0);   /* owner-mic WiFi fallback via cloud room */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/roommic") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_room_mic(a, on != 0);   /* expose the cloud room's voice mix as BSDR_RoomMic */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ownmiclocal") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_owner_mic_local(a, on != 0);      /* use this computer's mic as the owner mic */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/threed") == 0) {
        /* 2D->3D SBS config. Fields: mode 0/1/2, deepness 0..100, convergence -50..50, swap 0/1,
         * ai (helper command). Reopens the capture on the next streamer tick (app bumps threed_gen). */
        double mode = 0, deep = -1, conv = 0, swap = 0, full = 1, tier = -1; char ai[256] = "";
        int have_ai = bsdr_json_get_str(body, "ai", ai, sizeof(ai));
        bsdr_json_get_double(body, "mode", &mode);
        bsdr_json_get_double(body, "deepness", &deep);
        bsdr_json_get_double(body, "convergence", &conv);
        bsdr_json_get_double(body, "swap", &swap);
        bsdr_json_get_double(body, "full", &full);
        bsdr_json_get_double(body, "tier", &tier);
        int cur_deep = 0, cur_tier = 0;
        bsdr_app_get_threed(a, NULL, &cur_deep, NULL, NULL, NULL, &cur_tier, NULL, 0);
        bsdr_app_set_threed(a, (int)mode, deep >= 0 ? (int)deep : cur_deep, (int)conv, swap != 0,
                            full != 0, tier >= 0 ? (int)tier : cur_tier, have_ai ? ai : NULL);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bitrate") == 0) {
        /* Video bitrate override. mbps>0 forces that bitrate (the headset's bitrate config is
         * ignored); 0 follows the headset. Applied live — the streamer reopens the encoder next tick. */
        double mbps = 0; bsdr_json_get_double(body, "mbps", &mbps);
        int bps = mbps > 0 ? (int)(mbps * 1000000.0 + 0.5) : 0;
        bsdr_app_set_bitrate_override(a, bps);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/encoder") == 0) {
        /* Encoder choice: gpu=1 -> CUDA/NVENC, gpu=0 -> CPU libx264 (sharper low-bitrate text).
         * Live-switchable (restarts a running session) and persisted across restarts. */
        double gpu = 0; bsdr_json_get_double(body, "gpu", &gpu);
        bsdr_app_set_gpu_encode(a, gpu != 0);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/vaapi") == 0) {
        /* VAAPI iGPU encode (Linux). Applied live (in-place capture reopen), persisted. */
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_vaapi(a, on != 0);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/kmsgrab") == 0) {
        /* kmsgrab DRM/KMS capture (Linux; needs CAP_SYS_ADMIN). Applied live, persisted. */
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_kmsgrab(a, on != 0);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/encmode") == 0) {
        /* Encoder effort level: 0 quality (default) / 1 balanced / 2 performance. Persisted; applies on
         * the next stream (re)start. Accept legacy "perf" (0/1) too. */
        double level = 0;
        if (bsdr_json_get_double(body, "level", &level) || bsdr_json_get_double(body, "perf", &level))
            bsdr_app_set_enc_level(a, (int)level);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/x264threads") == 0) {
        /* Opt-in x264 frame threads on the live --cpu path (P6.9). Persisted; next stream applies it. */
        double n = 1; bsdr_json_get_double(body, "n", &n);
        bsdr_app_set_x264_threads(a, (int)n);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fpscap") == 0) {
        /* Cap the capture/encode fps (0 = default 30). Persisted; applies on the next stream start. */
        double fps = 0; bsdr_json_get_double(body, "fps", &fps);
        bsdr_app_set_fps_cap(a, (int)fps);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/lan1x") == 0) {
        /* Send LAN video once (halve the uplink) vs twice. Persisted; takes effect live. */
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_lan_1x(a, on != 0);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wifiopt") == 0) {
        /* Wi-Fi network optimization: DSCP/WMM priority marking on the LAN media sockets. Persisted;
         * applies on the next stream start. */
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_wifi_opt(a, on != 0);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/model-import") == 0) {
        /* Import depth models from a zip that already exists on this machine (path given by the
         * operator). Extracts + verifies into the model cache. */
        char zip[1024] = ""; bsdr_json_get_str(body, "path", zip, sizeof(zip));
        int n = zip[0] ? bsdr_model_import_zip(zip) : -1;
        char out[48]; int ol = snprintf(out, sizeof(out), "{\"imported\":%d}", n);
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/model-download") == 0) {
        /* Kick off a background download of the selected built-in tier's depth model. Progress is
         * reported back through the status JSON (models.dl), polled by the UI. */
        double tier = 0; bsdr_json_get_double(body, "tier", &tier);
        int rc = bsdr_model_download_start((int)tier);
        char out[80];
        int ol = rc == 0 ? snprintf(out, sizeof(out), "{\"ok\":true}")
                         : snprintf(out, sizeof(out), "{\"ok\":false,\"err\":\"invalid tier or no URL (import the zip)\"}");
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/select") == 0) {
        char ip[64] = "";
        bsdr_json_get_str(body, "ip", ip, sizeof(ip));
        bsdr_app_select_quest(a, ip);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/source") == 0) {
        char mode[16] = "", p[512] = "", dev2[256] = "";
        bsdr_json_get_str(body, "mode", mode, sizeof(mode));
        bsdr_json_get_str(body, "path", p, sizeof(p));   /* file path/URL, or (webcam) the left camera */
        bsdr_app_set_source(a, mode[0] ? mode : NULL, p);
        if (bsdr_json_get_str(body, "dev2", dev2, sizeof(dev2)))   /* stereo: the right camera */
            bsdr_app_set_source_right(a, dev2);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/webcams") == 0) {
        /* Enumerate the platform's cameras for the source picker (Android does this in Kotlin). */
        bsdr_webcam_dev devs[16];
        int nd = bsdr_webcam_list(devs, 16);
        char out[2048]; int ol = snprintf(out, sizeof(out), "{\"cams\":[");
        for (int i = 0; i < nd && ol < (int)sizeof(out) - 256; i++) {
            char eid[300], enm[200];
            bsdr_json_escape(eid, sizeof eid, devs[i].id);
            bsdr_json_escape(enm, sizeof enm, devs[i].name);
            ol += snprintf(out + ol, sizeof(out) - ol, "%s{\"id\":\"%s\",\"name\":\"%s\"}",
                           i ? "," : "", eid, enm);
        }
        ol += snprintf(out + ol, sizeof(out) - ol, "]}");
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/pause") == 0) {
        bsdr_app_set_paused(a, !bsdr_app_is_paused(a));
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/disconnect") == 0) {
        bsdr_app_request_disconnect(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/share") == 0) {
        bsdr_app_set_internet_sharing(a, !bsdr_app_get_internet_sharing(a));
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sniff") == 0) {
        double want = 0; char pw[128] = "";
        bsdr_json_get_double(body, "want", &want);
        bsdr_json_get_str(body, "password", pw, sizeof(pw));   /* transient; feeds `sudo -S` */
        bsdr_app_set_sniff(a, want != 0, pw[0] ? pw : NULL);   /* capture method set via /api/sniffmethod */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sniffmethod") == 0) {
        /* capture method: 0 = passive sniff, 1 = MITM (ARP), 2 = router relay */
        double m = 0; bsdr_json_get_double(body, "method", &m);
        bsdr_app_set_sniff_method(a, (int)m);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/relayport") == 0) {
        double port = 0; bsdr_json_get_double(body, "port", &port);
        bsdr_app_set_relay_port(a, (int)port);   /* router-companion relay port (Android's owner-mic path) */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voicefx") == 0) {
        /* realtime Quest-mic voice change: master on + pitch/formant/volume + effects + substitute */
        double on = 1, gender = 0, formant = 0, volume = 0, robot = 0, echo = 0, whisper = 0, substitute = 0;
        bsdr_json_get_double(body, "on", &on);
        bsdr_json_get_double(body, "gender", &gender);
        bsdr_json_get_double(body, "formant", &formant);
        bsdr_json_get_double(body, "volume", &volume);
        bsdr_json_get_double(body, "robot", &robot);
        bsdr_json_get_double(body, "echo", &echo);
        bsdr_json_get_double(body, "whisper", &whisper);
        bsdr_json_get_double(body, "substitute", &substitute);
        bsdr_app_set_voicefx(a, on != 0, (int)gender, (int)formant, (int)volume,
                             (int)robot, (int)echo, (int)whisper, substitute != 0);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/faceswap") == 0) {
        /* realtime face swap: enable + tier + source-image path (server-side) */
        double on = 0, tier = 2; char src[512] = "";
        bsdr_json_get_double(body, "on", &on);
        bsdr_json_get_double(body, "tier", &tier);
        bsdr_json_get_str(body, "source", src, sizeof(src));
        bsdr_app_set_faceswap(a, on != 0, (int)tier, src);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/faceswap-download") == 0) {
        /* background-download the insightface models (buffalo_l + inswapper) into the faceswap dir.
         * Progress is reported through status.faceswap.models.dl, polled by the UI. */
        int rc = bsdr_faceswap_download_start();
        respond(c, 200, "application/json", rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}",
                rc == 0 ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/faceswap-import") == 0) {
        char zip[1024] = ""; bsdr_json_get_str(body, "path", zip, sizeof(zip));
        int n = zip[0] ? bsdr_faceswap_import_zip(zip) : -1;
        char out[48]; int ol = snprintf(out, sizeof(out), "{\"imported\":%d}", n);
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voiceai") == 0) {
        double on = 0, tier = 1, key = 0; char voice[64] = "";
        bsdr_json_get_double(body, "on", &on); bsdr_json_get_double(body, "tier", &tier);
        bsdr_json_get_double(body, "key", &key); bsdr_json_get_str(body, "voice", voice, sizeof voice);
        bsdr_app_set_voiceai(a, on != 0, (int)tier, voice, (int)key);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice-basedl") == 0) {
        int rc = bsdr_voice_base_download_start();
        respond(c, 200, "application/json", rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}", rc == 0 ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice-import") == 0) {
        char zip[1024] = ""; bsdr_json_get_str(body, "path", zip, sizeof zip);
        int n = zip[0] ? bsdr_voice_base_import_zip(zip) : -1;
        char out[48]; int ol = snprintf(out, sizeof out, "{\"imported\":%d}", n);
        respond(c, 200, "application/json", out, ol);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice-add-url") == 0) {
        char url[1024] = "", id[64] = "", name[96] = "";
        bsdr_json_get_str(body, "url", url, sizeof url); bsdr_json_get_str(body, "id", id, sizeof id);
        bsdr_json_get_str(body, "name", name, sizeof name);
        int rc = bsdr_voice_download_start(url, id, name);
        respond(c, 200, "application/json", rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}", rc == 0 ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice-add-file") == 0) {
        char p[1024] = "", id[64] = "", name[96] = "";
        bsdr_json_get_str(body, "path", p, sizeof p); bsdr_json_get_str(body, "id", id, sizeof id);
        bsdr_json_get_str(body, "name", name, sizeof name);
        int rc = (p[0] && id[0]) ? bsdr_voice_add_file(p, id, name, 40000, 1) : -1;
        respond(c, 200, "application/json", rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}", rc == 0 ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice-delete") == 0) {
        char id[64] = ""; bsdr_json_get_str(body, "id", id, sizeof id);
        if (id[0]) bsdr_voice_delete(id);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice-preset") == 0) {
        char act[16] = ""; double slot = 0; char name[64] = "";
        bsdr_json_get_str(body, "action", act, sizeof act); bsdr_json_get_double(body, "slot", &slot);
        bsdr_json_get_str(body, "name", name, sizeof name);
        int s = (int)slot;
        if (!strcmp(act, "save")) bsdr_app_voice_preset_save(a, s, name);
        else if (!strcmp(act, "apply")) bsdr_app_voice_preset_apply(a, s);
        else if (!strcmp(act, "delete")) bsdr_app_voice_preset_delete(a, s);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/windows") == 0) {
        bsdr_window wins[64], mons[16];
#ifdef BSDR_HAVE_CAPTURE
        int n  = bsdr_list_windows(getenv("DISPLAY"), wins, 64);
        int nm = bsdr_list_monitors(getenv("DISPLAY"), mons, 16);   /* single-screen "virtual desktops" */
#else
        int n = 0, nm = 0; (void)wins; (void)mons;
#endif
        /* Per entry: escaped title (each byte may double to 2, esc[] holds <=511) + the fixed
         * template + four ints (<=11 each). ~700 B/entry is a safe ceiling; size the buffer from
         * the real max instead of a fixed 320-per-entry that the old code also used (wrongly) as
         * each snprintf's size limit, which let a long window title overrun the heap. */
        if (n < 0) n = 0; else if (n > 64) n = 64;
        if (nm < 0) nm = 0; else if (nm > 16) nm = 16;
        size_t cap = 128 + (size_t)(n + nm) * 700;
        char *j = malloc(cap);
        if (!j) { respond(c, 500, "application/json", "{\"error\":\"oom\"}", 15); return; }
        size_t o = 0;
        /* every write is bounded by the true remaining space, and o only advances by bytes
         * actually written (snprintf returns would-have-written, which can exceed the limit). */
        #define WUI_APPEND(...) do { \
            int w_ = snprintf(j + o, cap - o, __VA_ARGS__); \
            if (w_ < 0) break; \
            o += ((size_t)w_ < cap - o) ? (size_t)w_ : (cap - o - 1); \
        } while (0)
        #define WUI_ESC(SRC, DST) do { size_t e_ = 0; \
            for (const char *s_ = (SRC); *s_ && e_ < sizeof(DST) - 2; s_++) { \
                if (*s_ == '"' || *s_ == '\\') (DST)[e_++] = '\\'; \
                if ((unsigned char)*s_ >= 0x20) (DST)[e_++] = *s_; } \
            (DST)[e_] = 0; } while (0)
        WUI_APPEND("[{\"title\":\"Whole desktop\",\"x\":0,\"y\":0,\"w\":0,\"h\":0}");
        /* one entry per monitor (a single screen = one "virtual desktop") */
        for (int i = 0; i < nm; i++) {
            char esc[512]; WUI_ESC(mons[i].title, esc);
            WUI_APPEND(",{\"title\":\"Screen: %s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                       esc, mons[i].x, mons[i].y, mons[i].w, mons[i].h);
        }
        /* then one entry per single window */
        for (int i = 0; i < n; i++) {
            char esc[512]; WUI_ESC(wins[i].title, esc);
            WUI_APPEND(",{\"title\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                       esc, wins[i].x, wins[i].y, wins[i].w, wins[i].h);
        }
        WUI_APPEND("]");
        #undef WUI_ESC
        #undef WUI_APPEND
        respond(c, 200, "application/json", j, o);
        free(j);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/browse", 11) == 0) {
        /* Server-side directory listing for the source picker (a browser file input can't hand us a
         * real path). Localhost + CSRF-guarded; lists only dirs the agent's own user can read, and
         * only media/.txt files. The `dir` value is untrusted -> just used as an opendir target
         * (no exec, no protocol handling); path traversal within the operator's own FS is expected. */
        char dir[512] = "";
        const char *q = strchr(path, '?');
        if (q) { const char *d = strstr(q, "dir="); if (d) url_decode(d + 4, dir, sizeof dir); }
        if (!dir[0]) { const char *hh = getenv("HOME"); snprintf(dir, sizeof dir, "%s", (hh && hh[0]) ? hh : "/"); }
        size_t dl = strlen(dir);
        while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;   /* strip trailing '/' (keep root) */
        char parent[512]; snprintf(parent, sizeof parent, "%s", dir);
        char *ps = strrchr(parent, '/');
        if (ps) { if (ps == parent) parent[1] = 0; else *ps = 0; }
        char edir[1040], epar[1040];
        bsdr_json_escape(edir, sizeof edir, dir);
        bsdr_json_escape(epar, sizeof epar, parent);
        size_t cap = 1u << 16;
        char *j = malloc(cap);
        if (!j) { respond(c, 500, "application/json", "{\"error\":\"oom\"}", 15); return; }
        size_t o = 0;
        #define WUI_APPEND(...) do { \
            int w_ = snprintf(j + o, cap - o, __VA_ARGS__); \
            if (w_ < 0) break; \
            o += ((size_t)w_ < cap - o) ? (size_t)w_ : (cap - o - 1); \
        } while (0)
        WUI_APPEND("{\"dir\":\"%s\",\"parent\":\"%s\",\"entries\":[", edir, epar);
        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *de; int first = 1, cnt = 0;
            while ((de = readdir(dp)) != NULL && cnt < 4000 && o < cap - 1100) {
                if (de->d_name[0] == '.') continue;   /* skip hidden and . / .. */
                char full[1200]; snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
                struct stat st; if (stat(full, &st) != 0) continue;
                int isdir = S_ISDIR(st.st_mode) ? 1 : 0;
                if (!isdir && !is_media_name(de->d_name)) continue;
                char en[1040]; bsdr_json_escape(en, sizeof en, de->d_name);
                WUI_APPEND("%s{\"name\":\"%s\",\"dir\":%s}", first ? "" : ",", en, isdir ? "true" : "false");
                first = 0; cnt++;
            }
            closedir(dp);
        }
        WUI_APPEND("]}");
        #undef WUI_APPEND
        respond(c, 200, "application/json", j, o);
        free(j);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/region") == 0) {
        double x = 0, y = 0, w = 0, h = 0;
        bsdr_json_get_double(body, "x", &x); bsdr_json_get_double(body, "y", &y);
        bsdr_json_get_double(body, "w", &w); bsdr_json_get_double(body, "h", &h);
        bsdr_app_set_region(a, (int)x, (int)y, (int)w, (int)h);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/voice") == 0) {
        char se[256]="", st[256]="", sm[64]="", le[256]="", lt[256]="", lm[64]="";
        bsdr_json_get_str(body, "stt", se, sizeof(se));
        bsdr_json_get_str(body, "sttToken", st, sizeof(st));
        bsdr_json_get_str(body, "sttModel", sm, sizeof(sm));
        bsdr_json_get_str(body, "llm", le, sizeof(le));
        bsdr_json_get_str(body, "llmToken", lt, sizeof(lt));
        bsdr_json_get_str(body, "llmModel", lm, sizeof(lm));
        bsdr_app_set_voice(a, se, st, sm, le, lt, lm);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/compctl") == 0) {
        double en = 0, vi = 0;
        bsdr_json_get_double(body, "enable", &en);
        bsdr_json_get_double(body, "vision", &vi);
        bsdr_app_set_compctl(a, en != 0, vi != 0);   /* main loop gates on sniffer+LLM before arming */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else {
        respond(c, 404, "text/plain", "not found", 9);
    }
}

/* case-insensitive request-header lookup: copies the value (no trailing CRLF) into out. */
static bool req_header(const char *buf, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *p = buf; *p && !(p[0] == '\r' && p[1] == '\n' && p[2] == '\r'); p++) {
        if ((p == buf || p[-1] == '\n') && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') v++;
            size_t o = 0;
            while (*v && *v != '\r' && *v != '\n' && o + 1 < cap) out[o++] = *v++;
            out[o] = '\0';
            return true;
        }
    }
    return false;
}

/* True if a Host/Origin header value names the loopback interface. Strips an optional scheme
 * ("http://") and matches the host component; the port is irrelevant. */
static bool host_is_local(const char *v) {
    const char *h = strstr(v, "://");
    h = h ? h + 3 : v;
    return strncmp(h, "127.0.0.1", 9) == 0 || strncasecmp(h, "localhost", 9) == 0 ||
           strncmp(h, "[::1]", 5) == 0;
}

/* CSRF / DNS-rebinding guard for the localhost-only panel. A browser always sends Host, and sends
 * Origin on cross-origin requests, so we reject when either is present and NOT loopback; both
 * absent (curl, our own same-origin fetches) is allowed. Blocks a malicious page from driving the
 * panel via fetch() and blocks a rebinding domain (Host = attacker's name) from reaching us. */
static bool webui_request_local(const char *buf) {
    char v[256] = "";
    if (req_header(buf, "Host", v, sizeof(v)) && !host_is_local(v)) return false;
    if (req_header(buf, "Origin", v, sizeof(v)) && !host_is_local(v)) return false;
    return true;
}

static void loop(void *arg) {
    struct bsdr_webui *w = (struct bsdr_webui *)arg;
    while (w->running) {
        bsdr_socket_t c = bsdr_tcp_accept(w->listener, NULL);
        if (c == BSDR_INVALID_SOCKET) {
            /* No pending connection. Sleep on the listener (up to 500ms) instead
             * of a 50Hz busy-poll: the panel is idle almost all the time, and the
             * timeout still lets us observe running=0 promptly on shutdown. */
            if (w->running) bsdr_socket_wait_readable(w->listener, 500);
            continue;
        }
        char *buf = malloc(8192);
        if (buf) {
            int total = 0, r;
            char *he = NULL;
            while (total < 8190 && (r = (int)recv(c, buf + total, 8190 - total, 0)) > 0) {
                total += r; buf[total] = '\0';
                if ((he = strstr(buf, "\r\n\r\n"))) {
                    long cl = 0; const char *clh = strstr(buf, "Content-Length:");
                    if (clh) cl = strtol(clh + 15, NULL, 10);
                    if ((long)(total - (he + 4 - buf)) >= cl) break;
                }
            }
            if (he) {
                char method[8] = "", path[256] = "";
                sscanf(buf, "%7s %255s", method, path);
                if (!webui_request_local(buf))
                    respond(c, 403, "application/json", "{\"error\":\"forbidden\"}", 20);
                else
                    handle(w, c, method, path, he + 4);
            }
            free(buf);
        }
        bsdr_socket_close(c);
    }
}

bsdr_webui *bsdr_webui_start(bsdr_app *app, uint16_t port) {
    struct bsdr_webui *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->app = app;
    w->listener = bsdr_tcp_listen("127.0.0.1", port, 8);
    if (w->listener == BSDR_INVALID_SOCKET) {
        BSDR_ERROR("bsdr.webui", "listen 127.0.0.1:%u failed", port);
        free(w); return NULL;
    }
    bsdr_set_nonblocking(w->listener);   /* so the accept loop can observe running=0 */
    w->running = 1;
    w->thread = bsdr_thread_start(loop, w);
    BSDR_INFO("bsdr.webui", "control UI at http://127.0.0.1:%u", port);
    return w;
}

void bsdr_webui_stop(bsdr_webui *w) {
    if (!w) return;
    w->running = 0;
    bsdr_socket_close(w->listener);
    if (w->thread) bsdr_thread_join(w->thread);
    free(w);
}
