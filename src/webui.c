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
"<div id=cloudshare class=row style='margin-top:12px;display:none'>"
"<span id=sharelbl class=grow>Internet sharing: off</span>"
"<button id=sharebtn class=p onclick=toggleShare()>Share to Internet</button></div></div>"

"<div class=card><h2>Headset (Quest)</h2>"
"<div id=quest class=status>...</div>"
"<div id=quests style=margin-top:6px></div>"
"<div class=hint>Pairing is automatic — the headset finds this PC on the LAN. "
"No pairing code to type.</div></div>"

"<div class=card><h2>Headset owner mic</h2>"
"<div id=sniff class=status>...</div>"
"<div class=hint>The Quest never sends its mic to this PC — the owner's voice only goes to the "
"Bigscreen room (cloud). This intercepts that stream off the LAN and exposes it as a virtual "
"microphone <b>BSDR-Quest-OwnerMic</b> (owner only). Passive capture needs this PC to see the "
"headset's traffic (be the gateway or a mirror port); <b>MITM</b> ARP-reroutes it through this PC "
"on a switched LAN. Needs the root password (used once for a helper, never stored).</div>"
"<div class=row><label style=width:auto><input id=snmitm type=checkbox style=width:auto> Use MITM (ARP)</label></div>"
"<div class=row><input id=snpw type=password class=grow placeholder='root (sudo) password — only if not already root'>"
"<button id=snbtn class=p onclick=toggleSniff()>Start owner mic</button></div>"
"<div class=hint><b>On Wi-Fi</b>, MITM can't work (the AP isolates stations). Run the "
"<b>router companion</b> <b>bsdr_micrelay</b> on your router — it captures the headset's uplink "
"there and forwards it here (start bsdrX with <code>--sniff-remote PORT</code>). Prebuilt binaries "
"for common routers are in <b>bsdrX_relay.zip</b>; see bsdr_micrelay(1).</div></div>"

"<div class=card><h2>Source</h2>"
"<div class=row>"
"<label><input type=radio name=src value=desktop onclick=src('desktop') style=width:auto> Desktop</label> "
"<label><input type=radio name=src value=file onclick=src('file') style=width:auto> Video file</label></div>"
"<div class=row><label style=width:auto;color:var(--muted)>Window</label>"
"<select id=win class=grow onchange=selWin()></select>"
"<button onclick=loadWindows() style=width:auto>&#8635;</button></div>"
"<div class=row><input id=path class=grow placeholder='/path/to/video.mp4, http(s)://… , rtsp://… , or playlist.txt' onchange=srcpath()>"
"<button onclick=openBrowse() style=width:auto>Browse&#8230;</button>"
"<button id=pause class=danger onclick=togglePause()>Stop</button></div>"
"<div class=hint>A video file, an <b>http/https/rtsp URL</b>, or a <b>.txt playlist</b> (one file "
"or URL per line, streamed in a loop). An in-VR media bar gives play/pause, seek, volume and exit; "
"the source's own audio streams too.</div>"
"<div class=row id=blankrow><label style=width:auto><input id=blank type=checkbox style=width:auto onchange=blankToggle()> "
"Blank my physical screen while the headset is connected (privacy)</label></div></div>"

"<div class=card><h2>2D&#8594;3D</h2>"
"<div class=hint>Convert the stream to side-by-side 3D in real time. <b>Fast</b> is a light "
"built-in depth heuristic (fine on old laptops); <b>AI</b> uses an external depth helper. Set your "
"Bigscreen screen to <b>SBS 3D</b> to see the effect. Uses the CPU encode path while on.</div>"
"<div class=row>"
"<label><input type=radio name=td value=0 onclick=threed() style=width:auto> Off</label> "
"<label><input type=radio name=td value=1 onclick=threed() style=width:auto> Fast</label> "
"<label><input type=radio name=td value=2 onclick=threed() style=width:auto> AI</label></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Deepness</label>"
"<input id=tddeep type=range min=0 max=100 oninput='tddv.textContent=tddeep.value' onchange=threed() class=grow>"
"<span id=tddv style=width:2.5em;text-align:right>35</span></div>"
"<div class=row><label style=width:120px;color:var(--muted)>Convergence</label>"
"<input id=tdconv type=range min=-50 max=50 oninput='tdcv.textContent=tdconv.value' onchange=threed() class=grow>"
"<span id=tdcv style=width:2.5em;text-align:right>0</span></div>"
"<div class=row><label style=width:auto><input id=tdfull type=checkbox style=width:auto onchange=threed()> Full resolution per eye (renders 3D at 2&#215; resolution, same screen shape; sharper, ~4&#215; encode cost)</label></div>"
"<div class=row><label style=width:auto><input id=tdswap type=checkbox style=width:auto onchange=threed()> Swap eyes (L/R)</label></div>"
"<div class=row><input id=tdai class=grow placeholder='e.g. python3 scripts/bsdr-depth-helper.py --model midas_v21_small.onnx' onchange=threed()></div>"
"<div class=hint><b>AI mode needs this command set</b> — a ready-to-use helper ships in "
"<code>scripts/bsdr-depth-helper.py</code> (real depth with a MiDaS-small ONNX model, or a built-in "
"heuristic if you omit <code>--model</code>). It reads a small grayscale frame on stdin and writes "
"back a depth map (header <code>BSDD</code>+w+h, then w&#215;h bytes each way), at a reduced rate. "
"With no command, or if it stalls, 3D falls back to Fast. See <b>bsdr_agent</b>(1).</div></div>"

"<div class=card><h2>Voice assistant</h2>"
"<div class=hint>Configure speech-to-text and (for spoken desktop commands) an LLM here. "
"When <b>computer control</b> is on, a movable balloon appears over the desktop in VR — click "
"it to speak; bsdrX listens until you stop, transcribes, and the LLM runs the action.</div>"

"<div class=sub2><div class=t>Speech-to-text (STT)<span id=sttbadge class='badge free'>free online service</span></div>"
"<div class=row><input id=se class=grow placeholder='leave blank = free online service'></div>"
"<div class=row><input id=sm placeholder=whisper-1 style=width:150px>"
"<input id=st type=password placeholder=token style=width:180px></div>"
"<div class=hint id=stthint>Leave the URL blank to use a built-in <b>free online</b> "
"transcription service — no setup. For private/faster results, point it at your own "
"Whisper server (e.g. <code>http://localhost:8080/inference</code>) or any OpenAI-compatible "
"<code>/v1/audio/transcriptions</code> endpoint, plus its model and token.</div></div>"

"<div class=sub2><div class=t>Language model (LLM)<span id=llmbadge class='badge custom'>not set</span></div>"
"<div class=row><input id=le class=grow placeholder='https://api.openai.com/v1/chat/completions'></div>"
"<div class=row><input id=lm placeholder=gpt-4o-mini style=width:150px>"
"<input id=lt type=password placeholder=token style=width:180px></div>"
"<div class=hint>Required for spoken <i>commands</i> (the model decides which desktop "
"action to run). Any OpenAI-compatible chat endpoint works. Without it, speech is only typed out.</div></div>"

"<div class=row style=margin-top:12px><button class=p onclick=voicecfg()>Apply voice settings</button></div>"

"<div class=sub2><div class=t>Computer control<span id=ccbadge class='badge custom'>off</span></div>"
"<div id=ccstatus class=status style=margin-bottom:8px>...</div>"
"<div class=hint>Turns the in-VR voice-command <b>balloon</b> on. Drag it anywhere over the "
"desktop; a click starts a listen-until-silence capture, then the LLM drives the desktop "
"(type text, key combos, click, scroll, launch apps). Requires the <b>owner mic</b> (sniffer or "
"MITM) running — the only source of your voice — and an LLM endpoint set above.</div>"
"<div class=row><label style=width:auto><input id=ccvis type=checkbox style=width:auto onchange=setVision()> "
"Vision (let the model take a desktop screenshot when a request needs it)</label></div>"
"<div class=row><label style=width:auto><input id=cloudmic type=checkbox style=width:auto onchange=cloudmicToggle()> "
"Owner mic via cloud room when the LAN sniffer/companion isn't available (WiFi). Isolates the owner "
"(loudest speaker) only while a command is spoken.</label></div>"
"<div class=row><button id=ccbtn class=p onclick=toggleCompctl()>Enable computer control</button></div></div>"
"</div>"

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
"function logout(){api('/api/logout',{})}"
"function sel(ip){api('/api/select',{ip:ip})}"
"function src(m){api('/api/source',{mode:m,path:path.value})}"
"function srcpath(){api('/api/source',{mode:'file',path:path.value})}"
"function blankToggle(){api('/api/blank',{on:blank.checked?1:0})}"
"function fbClose(){document.getElementById('fb').style.display='none'}"
"function showDonate(){document.getElementById('dn').style.display='block'}"
"function dnClose(){document.getElementById('dn').style.display='none'}"
"function dnCopy(b,a){let d=()=>{b.textContent='Copied';setTimeout(()=>b.textContent='Copy',1200)};"
"if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(a).then(d).catch(()=>{});}else{d();}}"
"function fbJoin(d,n){return d==='/'?'/'+n:d+'/'+n}"
"function openBrowse(){let p=path.value||'';let d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';fbBrowse(d)}"
"async function fbBrowse(d){let r=await api('/api/browse?dir='+encodeURIComponent(d));if(!r||r.dir==null)return;"
"document.getElementById('fb').style.display='block';document.getElementById('fbdir').textContent=r.dir;"
"let L=document.getElementById('fblist');L.innerHTML='';"
"let mk=(t,fn)=>{let b=document.createElement('div');b.textContent=t;b.style.cssText='padding:7px 9px;cursor:pointer;border-radius:6px';b.onmouseover=()=>b.style.background='rgba(255,255,255,.08)';b.onmouseout=()=>b.style.background='';b.onclick=fn;L.appendChild(b)};"
"if(r.parent!=null&&r.parent!==r.dir)mk('\\uD83D\\uDCC1 ..',()=>fbBrowse(r.parent));"
"(r.entries||[]).sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).forEach(e=>{"
"if(e.dir)mk('\\uD83D\\uDCC1 '+e.name,()=>fbBrowse(fbJoin(r.dir,e.name)));"
"else mk('\\uD83C\\uDFAC '+e.name,()=>fbPick(fbJoin(r.dir,e.name)))})}"
"function fbPick(f){path.value=f;srcpath();fbClose()}"
"async function loadWindows(){let ws=await api('/api/windows');let e=document.getElementById('win');e.innerHTML='';(ws||[]).forEach((o,i)=>{let p=document.createElement('option');p.value=i;p.textContent=o.title+(o.w?(' ('+o.w+'x'+o.h+')'):'');p.dataset.geo=JSON.stringify(o);e.appendChild(p)})}"
"function selWin(){let e=document.getElementById('win');let o=JSON.parse(e.options[e.selectedIndex].dataset.geo);api('/api/region',{x:o.x,y:o.y,w:o.w,h:o.h})}"
"function togglePause(){api('/api/pause',{toggle:true})}"
"function disconnect(){api('/api/disconnect',{})}"
"function toggleShare(){api('/api/share',{toggle:true})}"
"function toggleSniff(){let on=snbtn.dataset.on==='1';api('/api/sniff',{want:on?0:1,mitm:snmitm.checked?1:0,password:snpw.value}).then(()=>{snpw.value=''})}"
"function voicecfg(){api('/api/voice',{stt:se.value,sttModel:sm.value,sttToken:st.value,llm:le.value,llmModel:lm.value,llmToken:lt.value})}"
"function toggleCompctl(){let on=ccbtn.dataset.on==='1';api('/api/compctl',{enable:on?0:1,vision:ccvis.checked?1:0})}"
"function setVision(){let on=ccbtn.dataset.on==='1';api('/api/compctl',{enable:on?1:0,vision:ccvis.checked?1:0})}"
"function cloudmicToggle(){api('/api/cloudmic',{on:cloudmic.checked?1:0})}"
"function threed(){let m=0;for(const r of document.getElementsByName('td'))if(r.checked)m=+r.value;api('/api/threed',{mode:m,deepness:+tddeep.value,convergence:+tdconv.value,swap:tdswap.checked?1:0,full:tdfull.checked?1:0,ai:tdai.value})}"
"function dot(b){return '<span class=\"dot '+(b?'on':'off')+'\"></span>'}"
"async function tick(){let s=await api('/api/status');"
"cloud.innerHTML=dot(s.cloud.loggedIn)+'<span>'+(s.cloud.loggedIn?('Connected as '+s.cloud.name):('Not connected — '+s.cloud.msg))+'</span>'+(s.cloud.loggedIn?'<button onclick=logout() style=margin-left:auto>Log out</button>':'');"
"loginform.style.display=s.cloud.loggedIn?'none':'flex';"
"cloudshare.style.display=s.cloud.loggedIn?'flex':'none';"
"if(s.cloud.loggedIn){let sh=s.cloud.internetSharing;sharelbl.textContent='Internet sharing: '+(sh?'ON':'off');sharebtn.textContent=sh?'Stop sharing':'Share to Internet';sharebtn.className=sh?'danger':'p';}"
"quest.innerHTML=dot(s.quest.paired)+'<span>'+(s.quest.paired?('Connected: '+s.quest.name+' ('+s.quest.ip+')'):'No headset connected')+'</span>'+(s.quest.streaming?'<span class=pill>streaming</span>':'')+(s.quest.paired?'<button class=danger style=margin-left:auto onclick=disconnect()>Disconnect</button>':'');"
"if(s.sniff){let sn=s.sniff;sniff.innerHTML=dot(sn.active)+'<span>'+(sn.active?('On — '+(sn.msg||'active')):('Off'+(sn.msg?(' — '+sn.msg):'')))+'</span>';snbtn.dataset.on=sn.want?'1':'0';snbtn.textContent=sn.want?'Stop owner mic':'Start owner mic';snbtn.className=sn.want?'danger':'p';if(document.activeElement!==snmitm)snmitm.checked=sn.mitm;}"
"let h='';for(const q of s.quests){let on=s.selected===q.ip;h+=\"<div class=q><span class=nm>\"+(on?'\\u2713 ':'')+q.name+' <span style=color:var(--muted)>'+q.ip+\"</span></span><button onclick=\\\"sel('\"+q.ip+\"')\\\">\"+(on?'Selected':'Use')+'</button></div>'}"
"if(s.quests.length>1||s.selected)h='<div class=hint style=margin-top:8px>Choose a headset:</div>'+h;quests.innerHTML=h;"
"for(const r of document.getElementsByName('src'))r.checked=(r.value===s.source.mode);"
"if(document.activeElement!==path)path.value=s.source.path||'';"
"if(document.activeElement!==blank)blank.checked=!!s.blank;"
"blankrow.style.display=s.android?'none':'';"   /* screen-blank is desktop-only */
"if(document.activeElement!==cloudmic)cloudmic.checked=!!s.cloudMic;"
"if(s.threed){let t=s.threed;for(const r of document.getElementsByName('td'))r.checked=(+r.value===t.mode);"
"if(document.activeElement!==tddeep){tddeep.value=t.deepness;tddv.textContent=t.deepness;}"
"if(document.activeElement!==tdconv){tdconv.value=t.convergence;tdcv.textContent=t.convergence;}"
"if(document.activeElement!==tdswap)tdswap.checked=!!t.swap;"
"if(document.activeElement!==tdfull)tdfull.checked=t.full!==false;"
"if(document.activeElement!==tdai)tdai.value=t.ai||'';}"
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
"ccstatus.innerHTML=dot(cc.active)+'<span>'+(cc.active?('Armed — '+(cc.msg||'balloon active')):(cc.want?('Pending — '+(cc.msg||'waiting')):(ready?'Off — ready to enable':(s.android?'Off — set an LLM first':'Off — turn on the owner mic and set an LLM first'))))+'</span>';"
"ccbadge.textContent=cc.active?'armed':(cc.want?'pending':'off');ccbadge.className='badge '+(cc.active?'free':'custom');"
"if(document.activeElement!==ccvis)ccvis.checked=!!cc.vision;}"
"let pb=document.getElementById('pause');pb.textContent=s.quest.paused?'Restart':'Stop';pb.className=s.quest.paused?'p':'danger';}"
"setInterval(tick,1000);tick();loadWindows();"
"</script></body></html>";

static void respond(bsdr_socket_t c, int code, const char *ctype, const char *body, size_t blen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
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
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        static char json[8192];
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
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/blank") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_blank(a, on != 0);   /* takes effect while the Quest is connected */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cloudmic") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_cloud_mic_fallback(a, on != 0);   /* owner-mic WiFi fallback via cloud room */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/threed") == 0) {
        /* 2D->3D SBS config. Fields: mode 0/1/2, deepness 0..100, convergence -50..50, swap 0/1,
         * ai (helper command). Reopens the capture on the next streamer tick (app bumps threed_gen). */
        double mode = 0, deep = -1, conv = 0, swap = 0, full = 1; char ai[256] = "";
        int have_ai = bsdr_json_get_str(body, "ai", ai, sizeof(ai));
        bsdr_json_get_double(body, "mode", &mode);
        bsdr_json_get_double(body, "deepness", &deep);
        bsdr_json_get_double(body, "convergence", &conv);
        bsdr_json_get_double(body, "swap", &swap);
        bsdr_json_get_double(body, "full", &full);
        int cur_deep = 0; bsdr_app_get_threed(a, NULL, &cur_deep, NULL, NULL, NULL, NULL, 0);
        bsdr_app_set_threed(a, (int)mode, deep >= 0 ? (int)deep : cur_deep, (int)conv, swap != 0,
                            full != 0, have_ai ? ai : NULL);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/select") == 0) {
        char ip[64] = "";
        bsdr_json_get_str(body, "ip", ip, sizeof(ip));
        bsdr_app_select_quest(a, ip);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/source") == 0) {
        char mode[16] = "", p[512] = "";
        bsdr_json_get_str(body, "mode", mode, sizeof(mode));
        bsdr_json_get_str(body, "path", p, sizeof(p));
        bsdr_app_set_source(a, mode[0] ? mode : NULL, p);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
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
        double want = 0, mitm = 0; char pw[128] = "";
        bsdr_json_get_double(body, "want", &want);
        bsdr_json_get_double(body, "mitm", &mitm);
        bsdr_json_get_str(body, "password", pw, sizeof(pw));   /* transient; feeds `sudo -S` */
        bsdr_app_set_sniff(a, want != 0, mitm != 0, pw[0] ? pw : NULL);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/windows") == 0) {
        bsdr_window wins[64];
#ifdef BSDR_HAVE_CAPTURE
        int n = bsdr_list_windows(getenv("DISPLAY"), wins, 64);
#else
        int n = 0; (void)wins;
#endif
        /* Per entry: escaped title (each byte may double to 2, esc[] holds <=511) + the fixed
         * template + four ints (<=11 each). ~700 B/entry is a safe ceiling; size the buffer from
         * the real max instead of a fixed 320-per-entry that the old code also used (wrongly) as
         * each snprintf's size limit, which let a long window title overrun the heap. */
        if (n < 0) n = 0; else if (n > 64) n = 64;
        size_t cap = 128 + (size_t)n * 700;
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
        WUI_APPEND("[{\"title\":\"Whole desktop\",\"x\":0,\"y\":0,\"w\":0,\"h\":0}");
        for (int i = 0; i < n; i++) {
            char esc[512]; size_t e = 0;
            for (const char *s = wins[i].title; *s && e < sizeof(esc) - 2; s++) {
                if (*s == '"' || *s == '\\') esc[e++] = '\\';
                if ((unsigned char)*s >= 0x20) esc[e++] = *s;
            }
            esc[e] = 0;
            WUI_APPEND(",{\"title\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                       esc, wins[i].x, wins[i].y, wins[i].w, wins[i].h);
        }
        WUI_APPEND("]");
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
        if (c == BSDR_INVALID_SOCKET) { if (w->running) bsdr_sleep_ms(20); continue; }
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
