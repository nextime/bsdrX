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
#include "bsdr/cloud.h"
#include "bsdr/net.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/winlist.h"
#include "bsdr/model_store.h"
#include "bsdr/voicestore.h"
#include "bsdr/webcam.h"
#include "bsdr/deps.h"
#include "bsdr/tls.h"
#include "bsdr/roomcmd.h"    /* bsdr_roomcmd_status — live in-room bot state panel */
#include "bsdr/updatecheck.h" /* BSDR_UPDATE_HOME — new-release banner */
#include "bsdr/plugin.h"      /* loadable plugins: api/plugin dispatch + bot-card UI injection */
#include "bsdr/plugstore.h"   /* in-app plugin store: account/catalog/buy/download/enable */

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
/* File-name filter for the server-side browser, by target kind (?kind= in /api/browse):
 *   0 = media  — video containers + .txt playlists (the video Source picker)
 *   1 = onnx   — RVC voice / model files (the AI-voice "Add file" picker)
 *   2 = image  — face pictures (the faceswap source picker)
 * An unknown/absent kind falls back to media (the original behaviour). */
static int name_matches(const char *n, int kind) {
    static const char *media[] = { ".mp4",".mkv",".mov",".avi",".webm",".m4v",".ts",".h264",
                                   ".264",".flv",".wmv",".mpg",".mpeg",".m2ts",".txt", NULL };
    static const char *onnx[]  = { ".onnx", NULL };
    static const char *image[] = { ".jpg",".jpeg",".png",".webp",".bmp", NULL };
    const char **ext = kind == 1 ? onnx : kind == 2 ? image : media;
    for (int i = 0; ext[i]; i++) if (ci_suffix(n, ext[i])) return 1;
    return 0;
}

struct bsdr_webui {
    bsdr_app *app;
    bsdr_socket_t listener;
    bsdr_thread *thread;
    volatile int running;
    char allow[256];   /* comma-separated extra allowed Host/Origin values for the CSRF guard */
    int  allow_any;    /* allow == "*": accept any Host/Origin (trust the reverse proxy / network) */
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
".spin{display:inline-block;width:11px;height:11px;border:2px solid currentColor;border-right-color:transparent;border-radius:50%;vertical-align:-1px;animation:spin .7s linear infinite}"
"@keyframes spin{to{transform:rotate(360deg)}}"
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
/* Splash: covers the page instantly (rendered before any JS) so opening the app-window never shows a
 * blank/half-built panel; fades out on the first successful status poll (see tick()/updPoll). */
"<div id=splash style=\"position:fixed;inset:0;background:#12141a;color:#e6e8ee;display:flex;flex-direction:column;align-items:center;justify-content:center;z-index:9999;transition:opacity .45s\">"
"<div style='font-size:30px;font-weight:700;letter-spacing:1px'>bsdrX</div>"
"<div class=spin style='width:22px;height:22px;margin-top:18px'></div>"
"<div class=hint style='margin-top:12px'>Starting the control panel\xE2\x80\xA6</div></div>"
"<header><h1>bsdrX <small style='font-size:.5em;opacity:.55;font-weight:400'>v" BSDR_VERSION "</small></h1><span class=sub>Bigscreen Remote Desktop</span>"
"<a href='https://bigscreen.nexlab.net' target=_blank rel=noopener style=margin-left:auto>bigscreen.nexlab.net</a>"
"<button class=p style=width:auto onclick=showDonate()>&#9829; Donate</button></header>"
/* New-release banner: hidden until the hourly checker finds a strictly-newer version. */
"<div id=upbanner style='display:none;margin:0 0 12px;padding:10px 14px;border-radius:8px;"
"background:#1e3a2f;border:1px solid #2f6b52;color:#d6ffe9'>"
"&#10024; <b>A new version of bsdrX is available</b> \xE2\x80\x94 <span id=uplatest></span> "
"(you have v" BSDR_VERSION "). "
"<a id=uplink href='https://bigscreen.nexlab.net' target=_blank rel=noopener style='color:#8affc4;font-weight:600'>"
"Get the update &#8599;</a></div>"

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
"Use the headset pointer as a <b>touchpad</b> (real tap/drag touch events) instead of a mouse</label></div>"
"<div class=pslot data-slot=account></div></div>"

/* Second (bot) account — its own login + room-join. Joining your room as a 2nd participant makes the
 * headset send your owner mic even when you're otherwise alone (Room.participants > 1). */
"<div class=card><h2>Headset (Quest)</h2>"
"<div id=quest class=status>...</div>"
"<div id=quests style=margin-top:6px></div>"
"<div class=hint>Pairing is automatic — the headset finds this PC on the LAN. "
"No pairing code to type.</div></div>"
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
"<button id=botjoinbtn class=p onclick=botPrimary()>Join my room</button>"
"<span id=botfeedback class=hint style='margin-left:8px;align-self:center'></span>"
"<label class=hint style='margin-left:12px'>Presence "
"<select id=botmode onchange=botMode()></select></label>"
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
"<div id=botresetrow class=row style='margin-top:6px;display:none'>"
"<button onclick=botResetRoom() title='Kick everyone so they rejoin a fresh room'>Reset room</button>"
"<span class=hint style=margin-left:8px>clears a <b>stuck/frozen</b> room: removes all participants (official kick) so everyone rejoins fresh \xE2\x80\x94 nothing crashes.</span></div>"
"<div id=plugins style='margin-top:6px'></div>"   /* loadable plugins inject their bot-card UI here */
"<div class=hint style=margin-top:6px><b>audio only</b> is the bare built-in bot: it just sits in the "
"room so your mic works when you're alone — no avatar, no assistant. Other modes come from installed "
"plugins: the <b>fullbot</b> plugin adds <b>full bot</b> (avatar + the LLM assistant/moderation).</div>"
"<div class=row id=grprow style=display:none><label style=width:auto>Tool groups</label>"
"<label style='width:auto;font-weight:400'><input id=grp_public type=checkbox style=width:auto onchange='aclToggle(\"public\",grp_public.checked)'>public</label>"
"<label style='width:auto;font-weight:400'><input id=grp_moderator type=checkbox style=width:auto onchange='aclToggle(\"moderator\",grp_moderator.checked)'>moderator</label>"
"<label style='width:auto;font-weight:400'><input id=grp_botctl type=checkbox style=width:auto onchange='aclToggle(\"botctl\",grp_botctl.checked)'>bot control</label>"
"<label style='width:auto;font-weight:400'><input id=grp_computer type=checkbox style=width:auto onchange='aclToggle(\"computer\",grp_computer.checked)'>computer</label>"
"<label style='width:auto;font-weight:400'><input id=grp_admin type=checkbox style=width:auto onchange='aclToggle(\"admin\",grp_admin.checked)'>admin</label></div>"

/* In-room bot tiers + moderation — lives WITH the bot controls (not buried under Voice assistant) so
 * everything you configure for the bot is in one card: access levels, tool groups, volume policy,
 * overload, mic-check, friends/requests/bans, and a live view of who the bot currently hears. */
"<div class=sub2 id=aclcard style='margin-top:12px;display:none'><div class=t>In-room bot \xE2\x80\x94 access &amp; moderation<span class='badge custom'>tiers</span></div>"
"<div class=hint>When the bot is in your room it hears each person separately, resolves their level "
"(<b>owner \xE2\x96\xB8 host \xE2\x96\xB8 friend</b>) and grants only that level's tools. Friends are the bot account's "
"approved friends; a friend hosting the room is promoted to host. You control it from the in-VR "
"balloon; for the bot to talk with the room, load the <b>full bot</b> plugin.</div>"

/* No Personality or Wake word here: both belong to the fullbot plugin, which renders them in its own
 * section of this card. The core bot never speaks conversationally and is never addressed by name, so
 * a second copy would only drift from fullbot's. */

/* live state: who the bot hears right now + their level/volume, and overload/mic-check/translate */
"<div class=row style=margin-top:8px><b>Live</b><span id=botstatehdr class=hint style=margin-left:auto>\xE2\x80\x94</span></div>"
"<div id=botstate class=status>join your room with the bot to see who it hears</div>"

"<div class=row style=margin-top:8px>"
"<label style='margin-left:auto;width:auto;font-weight:400'><input id=aclfriend type=checkbox style=width:auto onchange='aclToggle(\"friends\",aclfriend.checked)'> friends can use the bot</label>"
"<label style='width:auto;font-weight:400'><input id=aclhost type=checkbox style=width:auto onchange='aclToggle(\"hosts\",aclhost.checked)'> hosts can moderate</label></div>"

/* volume policy + overload — the numbers that used to be hardcoded (owner 100% / guest 70% / >3 queued) */
"<div class=row><label style=width:auto>Volume</label>"
"<label style='width:auto;font-weight:400'>owner <input id=gowner type=number min=0 max=100 step=5 style=width:64px onchange=aclPolicy()> %</label>"
"<label style='width:auto;font-weight:400'>host/friend <input id=gguest type=number min=0 max=100 step=5 style=width:64px onchange=aclPolicy()> %</label>"
"<span class=hint>others are muted unless mic-checked</span></div>"
"<div class=row><label style=width:auto>Overload</label>"
"<label style='width:auto;font-weight:400'>serve owner-only past <input id=oload type=number min=1 max=20 step=1 style=width:64px onchange=aclPolicy()> queued requests</label></div>"

"<div class=row><label style='width:auto;font-weight:400'><input id=aclmic type=checkbox style=width:auto onchange='aclMic(aclmic.checked)'> auto age-check unknown joiners</label></div>"
"<div class=row><input id=aclws name=bsdr-websearch-endpoint autocomplete=off placeholder='web-search endpoint (optional; query appended as ?q=)' class=grow onchange=aclWs()>"
"<input id=aclwstok name=bsdr-websearch-token type=password autocomplete=new-password placeholder=token style=width:120px onchange=aclWs()></div>"
"<div class=row style=margin-top:6px><b>Friends</b></div><div id=aclfriends class=status>\xE2\x80\xA6</div>"
"<div class=row><input id=aclnu name=bsdr-friend-name autocomplete=off placeholder='friend name' style=max-width:160px><input id=aclns name=bsdr-friend-sid autocomplete=off placeholder='socialId (optional)' style=max-width:160px>"
"<button onclick=aclAdd()>Add friend</button></div>"
"<div class=row style=margin-top:6px><b>Friend requests</b><button onclick=aclReqs() style=margin-left:auto>Refresh</button></div>"
"<div id=aclreqs class=status>press refresh</div>"
"<div class=row style=margin-top:6px><b>Banned</b></div><div id=aclbans class=status>\xE2\x80\xA6</div></div>"
"<div class=pslot data-slot=bot></div>"
"</div>"

/* Plugin store — account (persistent), catalog, buy, download, enable/disable. Talks to the store via
 * the agent's /api/plugstore backend (server-to-server; no CORS needed). */
"<div class=card><h2>Plugin store</h2>"
"<div class=hint>Browse, buy and install bsdrX plugins. Your store login is remembered across restarts. "
"Downloaded plugins install for this machine and load on the next reload/restart.</div>"
"<div class=row style=margin-top:8px><label style=width:auto>Store</label>"
"<input id=psurl class=grow placeholder='https://\xE2\x80\xA6/bsdrxstore'>"
"<button onclick=psSetUrl() style=width:auto>Save</button></div>"
"<div id=psacct class=status style=margin-top:8px>\xE2\x80\xA6</div>"
"<div id=pslogin class=row style=margin-top:8px>"
"<input id=psemail class=grow placeholder='store email'> "
"<input id=pspw class=grow type=password placeholder=password> "
"<button class=p onclick=psLogin()>Log in</button>"
"<button onclick=psRegister() style=width:auto>Create account</button></div>"
/* Two more ways in, so EVERY store account works — OAuth users and admins included. A license key (minted
 * on the store's Account page) is the universal credential; the store button opens the site to sign in
 * (with a password OR Google/GitHub) and mint one. */
"<div id=pskeyrow class=row style=margin-top:6px>"
"<input id=pskey class=grow placeholder='\xE2\x80\xA6or paste a license key (bslk_\xE2\x80\xA6)'>"
"<button class=p onclick=psLoginKey()>Use key</button>"
"<button class=p onclick=psOpenStore() style=width:auto>Sign in / sign up in browser\xE2\x80\xA6</button></div>"
"<div class=hint id=pskeyhint>Prefer <b>Google</b> or <b>GitHub</b>? Click <b>Sign in / sign up in "
"browser</b> \xE2\x80\x94 it opens the store, you authenticate there (password, Google or GitHub), and the "
"agent connects automatically. Or paste a <b>license key</b> from the store Account page above.</div>"
"<div class=row style=margin-top:8px><b>Installed on this machine</b>"
"<button onclick=psRefresh() style=margin-left:auto>Refresh</button></div>"
"<div class=hint>Every plugin present here \xE2\x80\x94 built-in and store-installed. Toggle one off to unload "
"it live; back on to reload it.</div>"
/* These are LIST containers (one plugin per row), so they must stack vertically. .status is a
 * single-line flex row (display:flex) used for the account line — reusing it here laid every plugin
 * side-by-side. Keep the class for its font-size but force block display so the rows stack. */
"<div id=psplugins class=status style=display:block>\xE2\x80\xA6</div>"
"<div class=row style=margin-top:12px><b>Available plugins</b></div>"
"<div id=pscatalog class=status style=display:block>\xE2\x80\xA6</div>"
"<div class=pslot data-slot=store></div></div>"


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
"<input id=snpw name=bsdr-sudo type=password autocomplete=new-password class=grow placeholder='root (sudo) password — only if not already root'>"
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
"<div class=sub2 id=vsubcard><div class=t>Cloud voice substitution <span class=badge>relay</span></div>"
"<div class=hint style=margin-bottom:6px>Send your voice-changed / AI-converted mic into the room (the voice-changer plugin does the DSP/AI change). Needs the sniff/relay path active.</div>"
"<div class=row><label style=width:auto><input id=vsub type=checkbox style=width:auto onchange=voicesub()> "
#if defined(__ANDROID__)
"Substitute into the cloud (relay): stop the headset&#8217;s original voice and inject the changed (or AI-converted) audio</label></div>"
"<div class=hint>Requires <b>Relay</b> active (we rewrite the headset&#8594;cloud packets in "
#else
"Substitute into the cloud (MITM/relay): stop the headset&#8217;s original voice and inject the changed (or AI-converted) audio</label></div>"
"<div class=hint>Requires <b>MITM</b> or <b>Relay</b> active (we rewrite the headset&#8594;cloud packets in "
#endif
"flight via NFQUEUE) and the agent running with <b>root / CAP_NET_ADMIN</b> on Linux. Otherwise the change "
"is still heard on the local virtual mic and the cloud-room fallback.</div></div>"
/* Room mic: the OTHER participants' voices, consumed from the cloud room's SFU (micPort). Distinct
 * from BSDR_QuestMic (the headset owner's own voice, sniffed off the LAN). Needs an active cloud
 * session (Internet sharing on). */
"<div class=sub2><div class=t>Owner mic routing</div>"
"<div class=row><label style=width:auto><input id=ownmiclocal type=checkbox style=width:auto onchange=ownMicLocalToggle()> "
"Use <b>this computer&#8217;s microphone</b> as the Quest mic (no headset mic capture needed).</label></div>"
"<div class=row><label style=width:auto><input id=ownmicquestmic type=checkbox style=width:auto onchange=ownMicQuestMicToggle()> "
"Play the owner voice into the <b>BSDR_QuestMic</b> device (default off) &#8212; e.g. as a last-chance mic substitute.</label></div>"
"<div class=row><label style=width:auto><input id=cloudmic type=checkbox style=width:auto onchange=cloudmicToggle()> "
"Quest mic via the <b>cloud room</b> when the LAN sniffer/companion isn&#8217;t available (Wi-Fi client isolation); isolates the owner while a command is spoken.</label></div></div>"
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
"<label id=src3dopt style=display:none><input type=radio name=src value=webcam3d onclick=pickSrc('webcam3d') style=width:auto> Stereo 3D (2 cams)</label> "
"<label><input type=radio name=src value=terminal onclick=pickSrc('terminal') style=width:auto> Terminal</label></div>"
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
/* Terminal source: stream a shell to the headset (ideal on a headless box). Backend = pty (no X) or
 * xvfb (private Xvfb+xterm). The command + pty grid are optional. Shown only in terminal mode. */
"<div class=row id=termrow style=display:none><label style=width:auto;color:var(--muted)>Backend</label>"
"<select id=termbk class=grow onchange=termSet()><option value=pty>PTY (headless, no X)</option><option value=xvfb>Xvfb + xterm (X, full mouse)</option></select></div>"
"<div class=row id=termrow2 style=display:none><input id=termcmd class=grow placeholder='command to run (blank = your $SHELL)' onchange=termSrc()>"
"<input id=termsz style=width:110px placeholder='120x36' onchange=termSet()></div>"
"<div class=hint id=termhint style=display:none>Streams a shell/console to the headset with the Quest keyboard+mouse injected — works on a "
"<b>headless</b> machine. <b>PTY</b> renders an in-process terminal (no X needed); <b>Xvfb</b> runs a private X server + xterm "
"(needs <code>xvfb</code> + <code>xterm</code> installed). Size (cols&#215;rows) applies to the PTY backend. An in-VR bar gives EXIT&#8594;desktop.</div>"
"<div class=row id=filerow><input id=path class=grow placeholder='/path/to/video.mp4, http(s)://… , rtsp://… , or playlist.txt' onchange=srcpath()>"
"<button onclick=openBrowse() style=width:auto>Browse&#8230;</button>"
"<label style='width:auto;font-weight:400'><input id=floop type=checkbox style=width:auto onchange=floopSet()> loop</label>"
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
"<label style='width:auto;margin-left:12px' title='Force a keyframe when the SFU asks (RTCP PLI/FIR). Helps late internet-room joiners on servers that feed RTCP back.'><input type=checkbox id=cloudpli style=width:auto onchange=cloudPliSet()> cloud RTCP keyframe</label>"
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





/* Optional 3rd-party dependencies: shown only when this platform has any (empty on Linux-native/Android).
 * Each row: present ✓ / a How-to-install button linking to /deps/<id> (auto-install where automatable). */
"<div class=card id=depcard style=display:none><h2>Dependencies</h2>"
"<div class=hint>Optional external drivers/programs some features need. bsdrX bundles what its licence "
"allows; the rest link to the official installer with step-by-step instructions.</div>"
"<div id=deplist></div></div>"

/* Full top-level panels contributed by loadable plugins — placed at the END of the page (after all
 * built-in cards). TWO containers, and they must stay separate:
 *   #pluginmanaged — host-rendered panel_html + auto-rendered config cards. pluginUI() rewrites this
 *                    whole subtree on every status poll.
 *   #pluginpanels  — cards a plugin's ui_script appends itself. NEVER rewritten by the host: those
 *                    scripts are injected once and self-guard on their card's id, so rewriting this
 *                    container would delete the card with nothing left to re-create it. */
"<div id=pluginmanaged></div><div id=pluginpanels></div>"

/* server-side file-browser modal (fills the source path) */
"<div id=fb style=\"display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:99\" onclick=\"if(event.target==this)fbClose()\">"
"<div style=\"background:#1b1f2a;color:#e6e8ee;max-width:640px;margin:6vh auto;border-radius:10px;padding:14px;max-height:80vh;display:flex;flex-direction:column;border:1px solid #333\">"
"<div class=row style=align-items:center><b id=fbdir style=flex:1;overflow:hidden;text-overflow:ellipsis>/</b>"
"<button onclick=fbClose() style=width:auto>&#10005;</button></div>"
"<div id=fblist style=overflow:auto;margin-top:8px></div>"
"<div class=hint id=fbhint>Pick a video, or a .txt playlist.</div></div></div>"

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
"function _at(s){return (''+s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\"/g,'&quot;');}"
/* plugin config variable changed (auto-rendered form in a plugin's settings card) */
"function pcfg(p,k,v){api('/api/plugin-config',{plugin:p,key:k,value:''+v});}"
/* ---- plugin store ---- */
"var psSt=null,psCat=null,psDL={};"   /* psDL: slugs with a download in flight (button disabled) */
"function psRefresh(){api('/api/plugstore/status').then(function(x){psSt=x;psAcct();psRender();});api('/api/plugstore/catalog').then(function(x){psCat=x;psRender();});api('/api/plugins').then(psPlugins);}"
/* Installed-on-this-machine list: every plugin on disk (built-in + store), with a live enable/disable. */
"function psPlugins(list){var el=document.getElementById('psplugins');if(!el)return;if(!list||!list.length){el.textContent='No plugins found.';return;}"
"el.innerHTML=list.slice().sort(function(a,b){return a.name<b.name?-1:1;}).map(function(p){"
"var st=p.loaded?'loaded':(p.enabled?'enabled (reload to load)':'disabled');"
"var tag=p.builtin?' <span class=hint>built-in</span>':'';"
"return '<div class=row style=\"margin-top:6px;align-items:baseline\"><b>'+_at(p.name)+'</b>'+tag"
"+(p.version?(' <span class=hint>v'+_at(p.version)+'</span>'):'')"
"+' <span class=hint style=margin-left:6px>'+st+'</span><span style=margin-left:auto></span>'"
"+'<button onclick=\"psEnable(\\''+p.name+'\\','+(p.enabled?0:1)+')\">'+(p.enabled?'Disable':'Enable')+'</button></div>';}).join('');}"
"function psAcct(){if(!psSt)return;if(document.activeElement!==psurl)psurl.value=psSt.url||'';"
"var li=psSt.loggedIn;psacct.innerHTML=li?('\\u{1F7E2} Signed in'+(psSt.email?(' as '+_at(psSt.email)):'')+' <button style=margin-left:8px onclick=psLogout()>Log out</button>'):'Not signed in \\u2014 log in or create an account to buy and download plugins.';"
"pslogin.style.display=li?'none':'flex';}"
"function psInstalled(slug){if(!psSt||!psSt.installed)return null;for(var i=0;i<psSt.installed.length;i++)if(psSt.installed[i].name===slug)return psSt.installed[i];return null;}"
"function psRender(){var el=document.getElementById('pscatalog');if(!el)return;if(!psCat||!psCat.plugins){el.textContent=(psCat&&psCat.error)?('Store error: '+psCat.error):'\\u2026';return;}"
"if(!psCat.plugins.length){el.textContent='No plugins available.';return;}"
"el.innerHTML=psCat.plugins.map(function(p){var ins=psInstalled(p.slug);"
"var price=(p.visibility==='paid')?((p.price_cents/100).toFixed(2)+' '+(p.currency||'EUR')):(p.visibility==='public'?'free':p.visibility);"
"var cv=p.compatible_version,noBuild=(p.compatible===false);"
"var upd=(ins&&cv&&ins.version&&cv!==ins.version);"
"var b='<div class=row style=\"margin-top:6px;align-items:baseline\"><b>'+_at(p.name)+'</b>'"
"+' <span class=hint>v'+_at(p.latest_version||'?')+' \\u2014 '+price+(p.source_kind?(' \\u2014 '+p.source_kind):'')+'</span>';"
"if(upd)b+=' <span class=badge style=\"background:#1e3a2f;color:#8affc4\">update \\u2192 v'+_at(cv)+'</span>';"
"else if(noBuild&&!ins)b+=' <span class=hint>(no build for your ABI/platform)</span>';"
"b+='<span style=margin-left:auto></span>';"
"if(p.visibility==='paid'&&!p.entitled){b+='<button class=p onclick=\"psBuy(\\''+p.slug+'\\')\">Buy</button>';}"
"if((p.entitled||p.visibility==='public')&&!noBuild){b+=psDL[p.slug]?'<button disabled style=opacity:.7><span class=spin></span> Downloading\\u2026</button>':('<button class='+(upd||!ins?'p':'\\'\\'')+' onclick=\"psDownload(\\''+p.slug+'\\')\">'+(ins?(upd?'Update':'Reinstall'):'Install')+'</button>');}"
"if(ins){b+='<button onclick=\"psEnable(\\''+p.slug+'\\','+(ins.enabled?0:1)+')\">'+(ins.enabled?'Disable':'Enable')+'</button>';"
"b+='<button class=danger onclick=\"psRemove(\\''+p.slug+'\\')\">Remove</button>';}"
"b+='</div>';if(p.summary||ins)b+='<div class=hint style=\"margin:0 0 4px\">'+_at(p.summary||'')+(ins?(' \\u2014 '+(ins.version?('v'+_at(ins.version)+' '):'')+(ins.loaded?'loaded':(ins.enabled?'installed (reload to load)':'disabled'))):'')+'</div>';return b;}).join('');}"
"function psSetUrl(){api('/api/plugstore/url',{url:psurl.value}).then(psRefresh);}"
"function psLogin(){api('/api/plugstore/login',{email:psemail.value,password:pspw.value}).then(function(r){pspw.value='';if(!r.ok)alert('Sign-in failed: '+(r.error||''));psRefresh();});}"
"function psLoginKey(){var k=(document.getElementById('pskey').value||'').trim();if(!k){alert('Paste a license key first.');return;}api('/api/plugstore/login-key',{key:k}).then(function(r){if(!r.ok)alert('License key not accepted: '+(r.error||''));else document.getElementById('pskey').value='';psRefresh();});}"
/* Seamless browser sign-in / sign-up: open the store's agent-login page (password OR Google/GitHub there),
 * handing it a callback to THIS agent. After the user authenticates, the store mints a license key and
 * redirects the browser back to /api/plugstore/oauth-callback, which adopts it. We poll status meanwhile
 * so the panel flips to signed-in on its own. */
"function psOpenStore(){var u=(psSt&&psSt.url)||'';if(!u){alert('Set the store URL first.');return;}"
"var cb=location.origin+'/api/plugstore/oauth-callback';"
"window.open(u.replace(/\\/+$/,'')+'/auth/agent?cb='+encodeURIComponent(cb),'_blank');"
"var n=0,iv=setInterval(function(){n++;api('/api/plugstore/status').then(function(x){"
"if(x&&x.loggedIn){clearInterval(iv);psSt=x;psAcct();psRender();psRefresh();}else if(n>150){clearInterval(iv);}});},2000);}"
"function psRegister(){if((pspw.value||'').length<8){alert('Password must be at least 8 characters.');return;}api('/api/plugstore/register',{email:psemail.value,password:pspw.value}).then(function(r){pspw.value='';if(!r.ok)alert('Sign-up failed: '+(r.error||''));psRefresh();});}"
"function psLogout(){api('/api/plugstore/logout',{}).then(psRefresh);}"
"function psBuy(slug){api('/api/plugstore/buy',{slug:slug}).then(function(r){if(r.ok&&r.url)window.open(r.url,'_blank','noopener');else alert('Could not open the purchase page.');});}"
"function psDownload(slug){if(psDL[slug])return;psDL[slug]=1;psRender();"   /* guard the re-click, flip the button to a spinner immediately */
"api('/api/plugstore/download',{slug:slug}).then(function(r){delete psDL[slug];if(!r.ok)alert('Download failed: '+(r.error||''));psRefresh();});}"
"function psEnable(name,on){api('/api/plugstore/enable',{name:name,on:on}).then(function(r){if(!r.ok)alert(r.error||'failed');psRefresh();});}"
"function psRemove(name){if(!confirm('Remove plugin '+name+'?'))return;api('/api/plugstore/remove',{name:name}).then(function(r){if(!r.ok)alert(r.error||'failed');psRefresh();});}"
/* ---- plugin UI extension API (panels, sections, config, scripts) ---- */
"window.bsdrUI={_cbs:[],_loaded:{},api:api,onStatus:function(f){this._cbs.push(f);},"
"slot:function(n){return document.querySelector('.pslot[data-slot='+JSON.stringify(n)+']');},"
/* badge(card,on,text): the on/off pill in a card's header — the one that stays visible when the card
 * is collapsed. `card` is the element or its id; a plugin calls this for its own card so it reads like
 * every built-in one. on===null shows plain text with no on/off colouring. */
"badge:function(c,on,txt){if(typeof c==='string')c=document.getElementById(c);enApply(c&&c.querySelector('h2 .en'),on,txt);}};"
"function psCfgInput(g,v){var t=(v.type==='password')?'password':(v.type==='number'?'number':(v.type==='bool'?'checkbox':'text'));"
"var a=' data-pcfg-plugin='+JSON.stringify(g.plugin)+' data-pcfg-key='+JSON.stringify(v.key);"
"if(t==='checkbox')return '<input type=checkbox'+a+((v.value==='1'||v.value==='true')?' checked':'')+'>';"
"return '<input type='+JSON.stringify(t)+a+' value='+JSON.stringify(v.value||'')+'>';}"
"function psCfgCard(g){var rows=g.vars.map(function(v){return '<div class=row style=margin-top:6px><label style=width:auto>'+_at(v.label)+'</label>'+psCfgInput(g,v)+(v.help?('<span class=hint style=margin-left:8px>'+_at(v.help)+'</span>'):'')+'</div>';}).join('');"
"return '<div class=card data-plugin-config='+JSON.stringify(g.plugin)+'><h2>'+_at(g.plugin)+' \\u2014 settings</h2>'+rows+'</div>';}"
"function pluginUI(s){var pm=document.getElementById('pluginmanaged');"
"var _td3d=(s.pluginsLoaded||[]).indexOf('2d-3d')>=0;var _so=document.getElementById('src3dopt');if(_so)_so.style.display=_td3d?'':'none';"
"var _fb=(s.pluginsLoaded||[]).indexOf('fullbot')>=0;var _gr=document.getElementById('grprow');if(_gr)_gr.style.display=_fb?'':'none';"
"var _bm=(s.pluginsLoaded||[]).indexOf('bot-moderator')>=0;var _ac=document.getElementById('aclcard');if(_ac)_ac.style.display=_bm?'':'none';"
"if(pm){var html=(s.pluginPanels||[]).map(function(p){return '<div class=card data-plugin='+JSON.stringify(p.name)+'>'+p.html+'</div>';}).join('');"
"html+=(s.pluginConfig||[]).map(psCfgCard).join('');"
"if(!(document.activeElement&&pm.contains(document.activeElement))&&pm.innerHTML!==html)pm.innerHTML=html;}"
"var acc={};(s.pluginSections||[]).forEach(function(g){var tmp=document.createElement('div');tmp.innerHTML=g.html;"
"Array.prototype.forEach.call(tmp.querySelectorAll('[data-slot]'),function(node){var nm=node.getAttribute('data-slot');(acc[nm]=acc[nm]||[]).push(node.innerHTML);});});"
"document.querySelectorAll('.pslot').forEach(function(el){var nm=el.getAttribute('data-slot');var h=(acc[nm]||[]).join('');"
"if(el.innerHTML!==h&&!(document.activeElement&&el.contains(document.activeElement)))el.innerHTML=h;});"
/* Inject each ui_script once. A script's card lives in #pluginpanels and only the script can create
 * it, so when a plugin goes away (store disable/remove -> plugins reload) we can't take its card back
 * out — reload the page instead, which clears the orphan and re-runs the survivors' scripts. */
"var _ps=(s.pluginScripts||[]);"
"for(var _n in window.bsdrUI._loaded){if(!_ps.some(function(p){return p.name===_n;})){location.reload();return;}}"
"_ps.forEach(function(p){if(window.bsdrUI._loaded[p.name])return;window.bsdrUI._loaded[p.name]=1;var sc=document.createElement('script');sc.src=p.src;sc.async=true;document.body.appendChild(sc);});"
/* Wire any card that appeared since the last poll — a ui_script's own card, or a host-rendered panel
 * just re-created above — as a collapsible panel like every built-in one (no-ops on cards already
 * done). Must run BEFORE the callbacks: it creates the header badge span that a plugin's onStatus
 * then fills in via bsdrUI.badge(), so the badge lands on the same poll the card appears. Panel keys
 * come from the header text, so a card that used to be built-in (2D->3D, face swap) keeps its old
 * localStorage key and stays collapsed if that's how the user left it. */
"setupPanels();"
"window.bsdrUI._cbs.forEach(function(f){try{f(s);}catch(e){}});}"
"function login(){api('/api/login',{email:email.value,password:pw.value})}"
"function tlsToggle(){api('/api/tls',{insecure:tlsinsecure.checked?1:0})}"
"function logout(){api('/api/logout',{})}"
"function botLogin(){api('/api/bot/login',{email:botemail.value,password:botpw.value}).then(()=>{botpw.value=''})}"
"function botLogout(){api('/api/bot/logout',{})}"
"function botPrimary(){if(botjoinbtn.dataset.joined==='1')botLeave();else botJoin();}"
"function botLeave(){botjoinbtn.disabled=true;botjoinbtn.textContent='Leaving\\u2026';if(document.getElementById('botfeedback'))botfeedback.textContent='leaving the room\\u2026 (follow-me off)';api('/api/bot/leave',{}).then(()=>{botjoinbtn.disabled=false;if(document.getElementById('botfeedback'))botfeedback.textContent='left the room';})}"
"function botStop(){api('/api/bot/stop',{})}"
"function botStart(){api('/api/bot/start',{})}"
"function botMode(){api('/api/bot/mode',{mode:botmode.value})}"
"function botFollowSet(){api('/api/bot/follow',{on:botfollow.checked?1:0})}"
"function botResetRoom(){if(!confirm('Reset the room? This removes ALL participants so everyone rejoins a fresh room.'))return;api('/api/bot/resetroom',{}).then(r=>{if(document.getElementById('botfeedback'))botfeedback.textContent='reset room \\u2014 removed '+((r&&r.kicked)||0)+' participant(s)';})}"
"function botLoopSet(){api('/api/bot/loopback',{on:botloop.checked?1:0})}"
"function botSoloSet(){api('/api/bot/solo',{on:botsolo.checked?1:0})}"
"function botJoin(){botjoinbtn.disabled=true;botjoinbtn.textContent='Joining\\u2026';"
"let full=(document.getElementById('botmode')&&botmode.value!=='audio');"
"if(document.getElementById('botfeedback'))botfeedback.textContent='\\u23F3 sending join request\\u2026';"
"api('/api/bot/join',{}).then(r=>{botjoinbtn.disabled=false;"
"if(!r||!r.ok){botjoinbtn.textContent='Join my room';if(document.getElementById('botfeedback'))botfeedback.textContent='\\u26A0\\uFE0F join failed \\u2014 is your headset in a room? (see log)';alert('Bot room-join failed \\u2014 check that your headset is in a room and the bot is logged in (see debug log).');}"
"else if(document.getElementById('botfeedback'))botfeedback.textContent=full?'\\u23F3 joined \\u2014 bringing up avatar\\u2026':'\\u2705 joined (audio only \\u2014 pick \\u201Cfull bot\\u201D for an avatar)';})}"
"function sel(ip){api('/api/select',{ip:ip})}"
"let cams=[],camMode='desktop';"
"function srcRows(m){let cam=(m==='webcam'||m==='webcam3d');let term=(m==='terminal');"
"winrow.style.display=(m==='desktop')?'flex':'none';winhint.style.display=(m==='desktop')?'block':'none';filerow.style.display=(m==='file')?'flex':'none';"
"camrow.style.display=cam?'flex':'none';camrowR.style.display=(m==='webcam3d')?'flex':'none';camhint.style.display=cam?'block':'none';"
"termrow.style.display=term?'flex':'none';termrow2.style.display=term?'flex':'none';termhint.style.display=term?'block':'none';}"
"function pickSrc(m){camMode=m;srcRows(m);"
"if(m==='webcam'||m==='webcam3d'){loadCams().then(pickCam);}else if(m==='terminal'){termSet();termSrc();}else{api('/api/source',{mode:m,path:path.value});}}"
"function termParseSz(){let m=(termsz.value||'').match(/(\\d+)\\s*[x,]\\s*(\\d+)/);return m?{cols:+m[1],rows:+m[2]}:{cols:0,rows:0};}"
"function termSet(){let s=termParseSz();api('/api/terminal',{backend:termbk.value,cols:s.cols,rows:s.rows});}"
"function termSrc(){api('/api/source',{mode:'terminal',path:termcmd.value});}"
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
"function floopSet(){api('/api/fileloop',{on:floop.checked?1:0})}"
"function blankToggle(){api('/api/blank',{on:blank.checked?1:0})}"
"function pointerModeToggle(){api('/api/pointermode',{touch:ptouch.checked?1:0})}"
"function bitrateSet(){api('/api/bitrate',{mbps:+brate.value||0})}"
"function encoderSet(){api('/api/encoder',{gpu:+enc.value})}"
"function vaapiSet(){api('/api/vaapi',{on:vaapi.checked?1:0})}"
"function kmsgrabSet(){api('/api/kmsgrab',{on:kmsgrab.checked?1:0})}"
"function cloudPliSet(){api('/api/cloudpli',{on:cloudpli.checked?1:0})}"
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
/* fbTarget/fbKind live on window so a PLUGIN's ui_script (e.g. voice-changer's "Browse…" for an .onnx
 * voice) can set them before calling window.fbBrowse — a top-level `let` would be invisible to it, which
 * is why the voice browse used to show videos and never fill the plugin's path field. */
"window.fbTarget='file';window.fbKind='media';"
"var FBHINT={media:'Pick a video, or a .txt playlist.',onnx:'Pick an .onnx model file.',image:'Pick an image file.'};"
"function openBrowse(){window.fbTarget='file';window.fbKind='media';let p=path.value||'';let d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';fbBrowse(d)}"
"async function fbBrowse(d){let r=await api('/api/browse?kind='+window.fbKind+'&dir='+encodeURIComponent(d));if(!r||r.dir==null)return;"
"{var _h=document.getElementById('fbhint');if(_h)_h.textContent=FBHINT[window.fbKind]||'Pick a file.';}"
"document.getElementById('fb').style.display='block';document.getElementById('fbdir').textContent=r.dir;"
"let L=document.getElementById('fblist');L.innerHTML='';"
"let mk=(t,fn)=>{let b=document.createElement('div');b.textContent=t;b.style.cssText='padding:7px 9px;cursor:pointer;border-radius:6px';b.onmouseover=()=>b.style.background='rgba(255,255,255,.08)';b.onmouseout=()=>b.style.background='';b.onclick=fn;L.appendChild(b)};"
"if(r.parent!=null&&r.parent!==r.dir)mk('\\uD83D\\uDCC1 ..',()=>fbBrowse(r.parent));"
"(r.entries||[]).sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).forEach(e=>{"
"if(e.dir)mk('\\uD83D\\uDCC1 '+e.name,()=>fbBrowse(fbJoin(r.dir,e.name)));"
"else mk('\\uD83D\\uDCC4 '+e.name,()=>fbPick(fbJoin(r.dir,e.name)))})}"
"function fbBase(f){let s=(''+f).split('/').pop();let d=s.lastIndexOf('.');return d>0?s.slice(0,d):s;}"
"function fbPick(f){var tgt=window.fbTarget;var el=(tgt&&tgt!=='file')?document.getElementById(tgt):null;if(el){el.value=f;el.dispatchEvent(new Event('change'));var vi=document.getElementById('vaddid');if(tgt==='vaddpath'&&vi&&!vi.value)vi.value=fbBase(f);}else{path.value=f;srcpath();}fbClose()}"
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
"function voicesub(){api('/api/voicefx',{on:1,substitute:vsub.checked?1:0})}"
"function relayPortSet(){api('/api/relayport',{port:+relayport.value})}"
"function aesc(s){return (''+(s||'')).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
"function aclLoad(){fetch('/api/acl').then(r=>r.json()).then(d=>{"
"aclfriend.checked=d.friendAccess;aclhost.checked=d.hostAccess;aclmic.checked=d.micAuto;"
"if(document.activeElement!=aclws)aclws.value=d.webSearch||'';"
"if(document.activeElement!=gowner)gowner.value=(d.gainOwner!=null?d.gainOwner:100);"
"if(document.activeElement!=gguest)gguest.value=(d.gainGuest!=null?d.gainGuest:70);"
"if(document.activeElement!=oload)oload.value=(d.overload!=null?d.overload:3);"
"grp_public.checked=d.groups.public;grp_moderator.checked=d.groups.moderator;grp_botctl.checked=d.groups.botctl;grp_computer.checked=d.groups.computer;grp_admin.checked=d.groups.admin;"
"aclfriends.innerHTML=(d.friends||[]).map(f=>{let n=f.u||f.s;return aesc(n)+\" <a href=# onclick=\\\"aclDel('\"+aesc(n)+\"');return false\\\">remove</a>\"}).join('<br>')||'(none)';"
"aclbans.innerHTML=(d.bans||[]).map(b=>{let n=b.u||b.s;return aesc(n)+\" <a href=# onclick=\\\"aclUnban('\"+aesc(n)+\"');return false\\\">unban</a>\"}).join('<br>')||'(none)';"
"}).catch(()=>{})}"
"function aclWs(){api('/api/acl/websearch',{endpoint:aclws.value,token:aclwstok.value})}"
"function aclToggle(w,on){api('/api/acl/toggle',{which:w,enabled:on?1:0})}"
"function aclMic(on){api('/api/acl/miccheck',{on:on?1:0})}"
"function aclPolicy(){api('/api/acl/policy',{gainOwner:+gowner.value||0,gainGuest:+gguest.value||0,overload:+oload.value||3})}"
"function botStatePoll(){fetch('/api/bot/state').then(r=>r.json()).then(d=>{"
"let hdr=[];if(!d.enabled)hdr.push('router off');if(d.overloaded)hdr.push('\\u26a0 overloaded (owner-only)');else if(d.queued)hdr.push(d.queued+' queued');"
"if(d.micCheck)hdr.push('mic-check: '+(d.micUser||'?'));if(d.translate)hdr.push('translating\\u2192'+(d.translateLang||'?'));"
"botstatehdr.textContent=hdr.join(' \\u00b7 ')||(d.enabled?'listening':'\\u2014');"
"let p=d.people||[];if(!p.length){botstate.textContent='no one the bot can hear yet';return;}"
"botstate.innerHTML=p.map(x=>{let b=x.level==='owner'?'#4ade80':x.level==='host'?'#60a5fa':x.level==='friend'?'#a78bfa':'#888';"
"return '<span style=\\\"display:inline-block;min-width:120px\\\">'+aesc(x.name)+'</span> <b style=color:'+b+'>'+x.level+'</b>'+(x.host?' (host)':'')+' <span class=hint>vol '+x.gain+'%</span>';}).join('<br>');"
"}).catch(()=>{})}"
"function aclAdd(){if(!aclnu.value&&!aclns.value)return;api('/api/acl/friend',{op:'add',username:aclnu.value,socialId:aclns.value});aclnu.value='';aclns.value='';setTimeout(aclLoad,250)}"
"function aclDel(u){api('/api/acl/friend',{op:'remove',username:u});setTimeout(aclLoad,250)}"
"function aclUnban(k){api('/api/acl/ban',{key:k});setTimeout(aclLoad,250)}"
"function aclReqs(){aclreqs.innerHTML='\\u2026';fetch('/api/acl/requests').then(r=>r.json()).then(d=>{aclreqs.innerHTML=(d.requests||[]).map(r=>aesc(r.u||r.s)+\" <a href=# onclick=\\\"aclReq('\"+aesc(r.id)+\"','\"+aesc(r.u)+\"','\"+aesc(r.s)+\"',1);return false\\\">accept</a> <a href=# onclick=\\\"aclReq('\"+aesc(r.id)+\"','','',0);return false\\\">decline</a>\").join('<br>')||'(none)'}).catch(()=>{aclreqs.innerHTML='(error)'})}"
"function aclReq(id,u,s,ok){api('/api/acl/friendreq',{op:ok?'accept':'decline',id:id,username:u,socialId:s});setTimeout(aclReqs,500);if(ok)setTimeout(aclLoad,600)}"
"function roomMicToggle(){api('/api/roommic',{on:roommic.checked?1:0})}"
"function cloudmicToggle(){api('/api/cloudmic',{on:cloudmic.checked?1:0})}"
"function ownMicLocalToggle(){api('/api/ownmiclocal',{on:ownmiclocal.checked?1:0})}"
"function ownMicQuestMicToggle(){api('/api/ownmicquestmic',{on:ownmicquestmic.checked?1:0})}"
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
"function enApply(e,on,txt){if(!e)return;"
"if(on===null){e.className='en';e.textContent=txt||'';}else{e.className='en '+(on?'y':'n');e.textContent=txt||(on?'on':'off');}}"
"function setEn(k,on,txt){enApply(document.getElementById('en_'+k),on,txt);}"
"function hideSplash(){var sp=document.getElementById('splash');if(sp){sp.style.opacity=0;setTimeout(function(){if(sp.parentNode)sp.parentNode.removeChild(sp);},500);}}"
"async function tick(){let s=await api('/api/status');if(!s||s.error){return;}hideSplash();"   /* first real status -> drop the splash */
"cloud.innerHTML=dot(s.cloud.loggedIn)+'<span>'+(s.cloud.loggedIn?('Connected as '+s.cloud.name):('Not connected — '+s.cloud.msg))+'</span>'+(s.cloud.loggedIn?'<button onclick=logout() style=margin-left:auto>Log out</button>':'');"
"loginform.style.display=s.cloud.loggedIn?'none':'flex';"
"loginform2.style.display=s.cloud.loggedIn?'none':'flex';"
"if(s.bot){let b=s.bot,bi=b.loggedIn,fbld=(s.pluginsLoaded||[]).indexOf('fullbot')>=0;"
"let avm={connecting:'\\u23F3 avatar connecting\\u2026',up:'\\u{1F7E2} avatar shown',ghost:'\\u26A0\\uFE0F avatar not shown (data channel didn\\u2019t connect \\u2014 see log)'};"
"let av=(b.joined&&b.avatar&&b.avatar!=='off')?(' \\u2014 '+(avm[b.avatar]||b.avatar)):'';"
"bot.innerHTML=dot(bi)+'<span>'+(bi?('Bot: '+b.name+(b.joined?(' — in room ('+(b.room||'')+')'):'')+(b.msg?(' — '+b.msg):'')+av):('Not logged in'+(b.msg?(' — '+b.msg):'')))+'</span>';"
"botlogin.style.display=(!bi&&!b.stopped)?'flex':'none';"
"botstart.style.display=(!bi&&b.stopped)?'flex':'none';"
"botactions.style.display=bi?'flex':'none';"
"botfollowrow.style.display=(bi&&fbld)?'flex':'none';"
"botlooprow.style.display=(bi&&fbld)?'flex':'none';"
"botresetrow.style.display=(bi&&b.joined)?'flex':'none';"
"var _ph=document.getElementById('plugins');if(_ph){var _pl=(s.plugins||[]);_ph.style.display=(bi&&_pl.length)?'block':'none';var _h=_pl.map(function(p){return '<div class=row style=\"margin-top:6px\">'+p.html+'</div>';}).join('');if(_ph.innerHTML!==_h)_ph.innerHTML=_h;}"
"if(document.getElementById('botfollow')&&document.activeElement!==botfollow)botfollow.checked=!!b.follow;"
"if(document.getElementById('botloop')&&document.activeElement!==botloop)botloop.checked=!!b.loopback;"
"if(document.getElementById('botsolo')&&document.activeElement!==botsolo)botsolo.checked=!!b.solo;"
"if(b.modes&&document.getElementById('botmode')){var _sel=botmode,_want=(b.modes||['audio']).join(',');"
"if(_sel.dataset.modes!==_want){_sel.dataset.modes=_want;_sel.innerHTML='';(b.modes||['audio']).forEach(function(m){var o=document.createElement('option');o.value=m;o.textContent=(m==='audio'?'audio only (bare core)':(m==='fullbot'?'full bot (avatar + brain)':m));_sel.appendChild(o);});}"
"if(document.activeElement!==_sel)_sel.value=b.mode||'audio';}"
"if(document.getElementById('botjoinbtn')&&!botjoinbtn.disabled){botjoinbtn.dataset.joined=b.joined?'1':'0';botjoinbtn.textContent=b.joined?'Leave room':'Join my room';botjoinbtn.className=b.joined?'':'p';}}"
"if(document.activeElement!==tlsinsecure)tlsinsecure.checked=!!s.tlsInsecure;"
"cloudshare.style.display=s.cloud.loggedIn?'flex':'none';"
"if(s.cloud.loggedIn){let sh=s.cloud.internetSharing;sharelbl.textContent='Internet sharing: '+(sh?'ON':'off');sharebtn.textContent=sh?'Stop sharing':'Share to Internet';sharebtn.className=sh?'danger':'p';}"
"quest.innerHTML=dot(s.quest.paired)+'<span>'+(s.quest.paired?('Connected: '+s.quest.name+' ('+s.quest.ip+')'):'No headset connected')+'</span>'+(s.quest.streaming?'<span class=pill>streaming</span>':'')+(s.quest.paired?'<button class=danger style=margin-left:auto onclick=disconnect()>Disconnect</button>':'');"
"if(s.sniff){let sn=s.sniff;sniff.innerHTML=dot(sn.active)+'<span>'+(sn.active?('On — '+(sn.msg||'active')):('Off'+(sn.msg?(' — '+sn.msg):'')))+'</span>';snbtn.dataset.on=sn.want?'1':'0';snbtn.textContent=sn.want?'Stop mic':'Start mic';snbtn.className=sn.want?'danger':'p';"
"if(document.activeElement!==snmethod&&sn.method!==undefined){snmethod.value=sn.method;snmethod.dataset.cur=sn.method;}"
"if(sn.wifi!==undefined)snmethod.dataset.wifi=sn.wifi?'1':'0';"
"relayrow.style.display=(sn.method===2)?'flex':'none';"
"if(document.activeElement!==relayport&&sn.relayPort!==undefined)relayport.value=sn.relayPort||'';"
"if(document.activeElement!==vsub&&sn.substitute!==undefined)vsub.checked=sn.substitute;}"
"let h='';for(const q of s.quests){let on=s.selected===q.ip;h+=\"<div class=q><span class=nm>\"+(on?'\\u2713 ':'')+q.name+' <span style=color:var(--muted)>'+q.ip+\"</span></span><button onclick=\\\"sel('\"+q.ip+\"')\\\">\"+(on?'Selected':'Use')+'</button></div>'}"
"if(s.quests.length>1||s.selected)h='<div class=hint style=margin-top:8px>Choose a headset:</div>'+h;quests.innerHTML=h;"
"for(const r of document.getElementsByName('src'))r.checked=(r.value===s.source.mode);"
"if(document.activeElement!==path)path.value=(s.source.mode==='file'?(s.source.path||''):path.value);"
"if(document.getElementById('floop')&&document.activeElement!==floop&&s.source.fileLoop!==undefined)floop.checked=!!s.source.fileLoop;"
"if(document.getElementById('termbk')&&document.activeElement!==termbk&&s.source.termBackend)termbk.value=s.source.termBackend;"
"if(document.getElementById('termsz')&&document.activeElement!==termsz&&s.source.termCols)termsz.placeholder=s.source.termCols+'x'+s.source.termRows;"
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
"if(document.activeElement!==cloudpli&&s.quality.cloudPli!==undefined)cloudpli.checked=!!s.quality.cloudPli;"
"if(document.activeElement!==encmode&&s.quality.encLevel!==undefined)encmode.value=''+s.quality.encLevel;"
"if(document.activeElement!==x264t&&s.quality.x264Threads!==undefined)x264t.value=''+(s.quality.x264Threads||1);"
"if(document.activeElement!==fpscap&&s.quality.fpsCap!==undefined)fpscap.value=s.quality.fpsCap;"
"if(document.activeElement!==lan1x&&s.quality.lan1x!==undefined)lan1x.checked=s.quality.lan1x;"
"if(document.activeElement!==wifiopt&&s.quality.wifiOpt!==undefined)wifiopt.checked=s.quality.wifiOpt;}"
"blankrow.style.display=s.android?'none':'';"   /* screen-blank is desktop-only */
"if(document.activeElement!==roommic)roommic.checked=!!s.roomMic;"
"if(document.activeElement!==cloudmic)cloudmic.checked=!!s.cloudMic;"
"if(document.activeElement!==ownmiclocal)ownmiclocal.checked=!!s.ownerMicLocal;"
"if(document.getElementById('ownmicquestmic')&&document.activeElement!==ownmicquestmic)ownmicquestmic.checked=!!s.ownerMicToQuestMic;"
"{let sh=s.cloud&&s.cloud.internetSharing;roommicstat.innerHTML=dot(!!s.roomMic&&sh)+'<span>'+(!s.roomMic?'Off':(sh?'On — consuming the room voice (BSDR_RoomMic)':'Waiting — turn on Internet sharing to reach the room'))+'</span>';}"
"let pb=document.getElementById('pause');pb.textContent=s.quest.paused?'Restart':'Stop';pb.className=s.quest.paused?'p':'danger';"
/* collapsed-panel enabled/disabled badges (computed from the same status) */
"setEn('bot',!!(s.bot&&s.bot.loggedIn),s.bot&&s.bot.loggedIn?(s.bot.joined?'in room':'on'):'off');"
"setEn('quest',!!s.quest.paired,s.quest.paired?(s.quest.streaming?'streaming':'connected'):'off');"
"setEn('mic',!!((s.sniff&&(s.sniff.active||s.sniff.fxOn))||s.roomMic||(s.voiceai&&s.voiceai.on)),"
"(s.sniff&&s.sniff.active)?'mic on':(((s.sniff&&s.sniff.fxOn)||(s.voiceai&&s.voiceai.on))?'voice fx':(s.roomMic?'room mic':'off')));"
"setEn('source',null,(s.source&&s.source.mode)?s.source.mode:'');"
"try{pluginUI(s);}catch(e){}"
"}"
"function updPoll(){fetch('/api/update').then(r=>r.json()).then(d=>{if(d&&d.available){uplatest.textContent='v'+(d.latest||'');if(d.home)uplink.href=d.home;upbanner.style.display='block';}}).catch(()=>{})}"
/* one delegated listener persists a plugin config var edited in an auto-rendered form (data-pcfg-*) */
"document.addEventListener('change',function(e){var t=e.target;if(t&&t.dataset&&t.dataset.pcfgKey)pcfg(t.dataset.pcfgPlugin,t.dataset.pcfgKey,(t.type==='checkbox')?(t.checked?1:0):t.value);});"
"setTimeout(hideSplash,8000);"   /* safety net: never leave the splash stuck if status is slow */
"setupPanels();setInterval(tick,1000);tick();loadWindows();loadDeps();aclLoad();setInterval(botStatePoll,2000);botStatePoll();updPoll();setInterval(updPoll,600000);psRefresh();"
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

/* Public thin wrapper so a loadable plugin's http() hook can reply on the request socket without the
 * host exporting `respond`. `conn` points at the bsdr_socket_t handed to bsdr_plugins_http below. */
void bsdr_webui_plugin_respond(void *conn, int code, const char *ctype, const char *body, size_t len) {
    if (!conn) return;
    respond(*(bsdr_socket_t *)conn, code, ctype ? ctype : "application/json", body ? body : "", len);
}

/* Build the access-control status JSON (friends, bans, toggles, groups, web-search, mic
 * check) into buf. Returns the length. Local data only — no network (friend requests are separate). */
static int acl_status_json(bsdr_app *a, char *buf, size_t cap) {
    bsdr_acl_entry fr[BSDR_ACL_MAX_FRIENDS], bn[BSDR_ACL_MAX_BANS];
    int nf = bsdr_acl_friends(a->acl, fr, BSDR_ACL_MAX_FRIENDS);
    int nb = bsdr_acl_bans(a->acl, bn, BSDR_ACL_MAX_BANS);
    if (nf > BSDR_ACL_MAX_FRIENDS) nf = BSDR_ACL_MAX_FRIENDS;
    if (nb > BSDR_ACL_MAX_BANS)    nb = BSDR_ACL_MAX_BANS;
    char ws[256] = ""; bool mca; int govr, ggst, othr;
    bsdr_mutex_lock(a->lock); snprintf(ws, sizeof ws, "%s", a->web_search_endpoint); mca = a->mic_check_auto;
    govr = (int)(a->gain_owner * 100 + 0.5f); ggst = (int)(a->gain_guest * 100 + 0.5f); othr = a->overload_threshold;
    bsdr_mutex_unlock(a->lock);
    uint32_t gm = bsdr_acl_group_mask(a->acl);
    char wse[512];
    bsdr_json_escape(wse, sizeof wse, ws);
    int o = snprintf(buf, cap,
        "{\"friendAccess\":%s,\"hostAccess\":%s,\"webSearch\":\"%s\",\"micAuto\":%s,"
        "\"gainOwner\":%d,\"gainGuest\":%d,\"overload\":%d,"
        "\"groups\":{\"public\":%d,\"moderator\":%d,\"botctl\":%d,\"computer\":%d,\"admin\":%d},\"friends\":[",
        bsdr_acl_friend_access(a->acl) ? "true" : "false", bsdr_acl_host_access(a->acl) ? "true" : "false",
        wse, mca ? "true" : "false", govr, ggst, othr,
        !!(gm & BSDR_TG_PUBLIC), !!(gm & BSDR_TG_MODERATOR), !!(gm & BSDR_TG_BOTCTL),
        !!(gm & BSDR_TG_COMPUTER), !!(gm & BSDR_TG_ADMIN));
    for (int i = 0; i < nf && o < (int)cap - 300; i++) {
        char u[128], s[160]; bsdr_json_escape(u, sizeof u, fr[i].username); bsdr_json_escape(s, sizeof s, fr[i].social_id);
        o += snprintf(buf + o, cap - o, "%s{\"u\":\"%s\",\"s\":\"%s\"}", i ? "," : "", u, s);
    }
    o += snprintf(buf + o, cap - o, "],\"bans\":[");
    for (int i = 0; i < nb && o < (int)cap - 300; i++) {
        char u[128], s[160]; bsdr_json_escape(u, sizeof u, bn[i].username); bsdr_json_escape(s, sizeof s, bn[i].social_id);
        o += snprintf(buf + o, cap - o, "%s{\"u\":\"%s\",\"s\":\"%s\"}", i ? "," : "", u, s);
    }
    o += snprintf(buf + o, cap - o, "]}");
    return o;
}

static void handle(struct bsdr_webui *w, bsdr_socket_t c, const char *method,
                   const char *path, const char *body) {
    bsdr_app *a = w->app;
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strncmp(path, "/?", 2) == 0)) {
        /* also serve "/" with a query (e.g. the app-window's /?app=1) — the query is just a UI hint. */
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
        /* Room for faceswap + voiceai objects AND plugin-contributed panels/config (own HTML/JS). */
        static char json[131072];
        size_t n = bsdr_app_status_json(a, json, sizeof(json));
        respond(c, 200, "application/json", json, n);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/update") == 0) {
        bsdr_mutex_lock(a->lock);
        bool up = a->update_available; char latest[32]; snprintf(latest, sizeof latest, "%s", a->update_latest);
        bsdr_mutex_unlock(a->lock);
        char le[64]; bsdr_json_escape(le, sizeof le, latest);
        char j[256];
        int n = snprintf(j, sizeof j,
            "{\"available\":%s,\"latest\":\"%s\",\"current\":\"%s\",\"home\":\"%s\"}",
            up ? "true" : "false", le, BSDR_VERSION, BSDR_UPDATE_HOME);
        respond(c, 200, "application/json", j, n);
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
        /* An explicit user Leave also cancels follow-me — otherwise the next follow poll would just
         * re-join the operator's room and the Leave would appear to do nothing. (follow-me's own
         * room changes call bsdr_app_bot_leave_room directly, so they are unaffected.) Turn follow
         * off FIRST so a concurrent follow tick can't re-join between the leave and the toggle. */
        bsdr_app_set_bot_follow(a, false);
        bool ok = bsdr_app_bot_leave_room(a);         /* leave the room, stay logged in */
        respond(c, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/stop") == 0) {
        bsdr_app_bot_stop(a);                         /* disconnect but remember the login */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/start") == 0) {
        bsdr_app_bot_start(a);                         /* reconnect from the remembered session */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/bot/resetroom") == 0) {
        /* Owner: clear the room (kick all) so everyone rejoins a fresh room — recovers a stuck room. */
        int n = bsdr_app_reset_room(a);
        char j[64]; int jn = snprintf(j, sizeof j, "{\"ok\":true,\"kicked\":%d}", n);
        respond(c, 200, "application/json", j, jn);
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
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ownmicquestmic") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_owner_mic_to_questmic(a, on != 0);   /* render owner voice into BSDR_QuestMic (default off) */
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
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cloudpli") == 0) {
        /* Opt-in: force an IDR when the SFU sends an RTCP keyframe request (helps late cloud joiners
         * on servers that feed RTCP back). Applied live to the running cloud stream. */
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_mutex_lock(a->lock); a->cloud_rtcp_pli = on != 0; bsdr_mutex_unlock(a->lock);
        bsdr_app_save_settings(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fileloop") == 0) {
        /* Loop the file/playlist source continuously (also toggleable from the in-VR media bar). */
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_app_set_file_loop(a, on != 0);
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
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/terminal") == 0) {
        /* Terminal-source backend + pty grid (persisted). Does NOT switch the source itself — the UI
         * calls /api/source {mode:terminal} for that. */
        char bk[8] = ""; double cols = 0, rows = 0;
        bsdr_json_get_str(body, "backend", bk, sizeof(bk));
        bsdr_json_get_double(body, "cols", &cols);
        bsdr_json_get_double(body, "rows", &rows);
        bsdr_app_set_terminal(a, bk[0] ? bk : NULL, (int)cols, (int)rows);
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
        int browse_kind = 0;   /* 0=media, 1=onnx, 2=image — see name_matches() */
        if (q) { const char *k = strstr(q, "kind=");
                 if (k) { k += 5; if (!strncmp(k, "onnx", 4)) browse_kind = 1;
                                   else if (!strncmp(k, "image", 5)) browse_kind = 2; } }
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
                if (!isdir && !name_matches(de->d_name, browse_kind)) continue;
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
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/llmctx") == 0) {
        bsdr_mutex_lock(a->lock);
        int ov = a->llm_context_tokens, det = a->llm_context_detected;
        int pct = a->llm_compact_pct, strat = a->llm_compact_strategy, mr = a->llm_max_rounds;
        bsdr_mutex_unlock(a->lock);
        int eff = ov > 0 ? ov : (det > 0 ? det : 32768);
        char j[256];
        int n = snprintf(j, sizeof j,
            "{\"context\":%d,\"detected\":%d,\"effective\":%d,\"compactOn\":%s,\"compactPct\":%d,"
            "\"strategy\":%d,\"maxRounds\":%d}",
            ov, det, eff, pct >= 0 ? "true" : "false", pct >= 0 ? pct : 80, strat, mr);
        respond(c, 200, "application/json", j, n);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/llmctx") == 0) {
        double ctx = -1, on = -1, pct = -1, strat = -1, mr = -1;
        bsdr_json_get_double(body, "context", &ctx);
        bsdr_json_get_double(body, "compactOn", &on);
        bsdr_json_get_double(body, "compactPct", &pct);
        bsdr_json_get_double(body, "strategy", &strat);
        bsdr_json_get_double(body, "maxRounds", &mr);
        bsdr_mutex_lock(a->lock);
        if (ctx >= 0) a->llm_context_tokens = (int)ctx;
        if (on == 0)  a->llm_compact_pct = -1;                             /* off */
        else if (pct >= 10 && pct <= 100) a->llm_compact_pct = (int)pct;   /* on at pct */
        if (strat >= 0 && strat <= 2) a->llm_compact_strategy = (int)strat;
        if (mr >= 0 && mr <= 200) a->llm_max_rounds = (int)mr;
        bsdr_mutex_unlock(a->lock);
        bsdr_app_save_settings(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/tts") == 0) {
        double on = 0, engine = 0, route = 0;
        bsdr_json_get_double(body, "on", &on);
        bsdr_json_get_double(body, "engine", &engine);
        bsdr_json_get_double(body, "route", &route);
        bsdr_tts_config cfg; memset(&cfg, 0, sizeof cfg);
        cfg.engine = (engine != 0) ? BSDR_TTS_CLOUD : BSDR_TTS_LOCAL;
        bsdr_json_get_str(body, "piper",      cfg.piper,       sizeof cfg.piper);
        bsdr_json_get_str(body, "model",      cfg.model,       sizeof cfg.model);
        bsdr_json_get_str(body, "endpoint",   cfg.endpoint,    sizeof cfg.endpoint);
        bsdr_json_get_str(body, "cloudModel", cfg.cloud_model, sizeof cfg.cloud_model);
        bsdr_json_get_str(body, "voice",      cfg.voice,       sizeof cfg.voice);
        bsdr_json_get_str(body, "token",      cfg.token,       sizeof cfg.token);  /* blank = keep existing */
        bsdr_app_set_tts(a, &cfg, on != 0, (int)route);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/tts/say") == 0) {
        /* Manual "say to room" trigger: make the bot speak this line now (also the way to test the
         * TTS -> room-mic path without the LLM). Non-blocking — the app queues + paces it. */
        char text[512] = "";
        bsdr_json_get_str(body, "text", text, sizeof text);
        bsdr_app_tts_say(a, text);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/compctl") == 0) {
        double en = 0, vi = 0;
        bsdr_json_get_double(body, "enable", &en);
        bsdr_json_get_double(body, "vision", &vi);
        bsdr_app_set_compctl(a, en != 0, vi != 0);   /* main loop gates on sniffer+LLM before arming */
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/browserctl") == 0) {
        /* Owner-only browser control via CDP (default OFF). Enable + set the DevTools endpoint. */
        double on = -1; bsdr_json_get_double(body, "on", &on);
        char ep[256] = ""; bool have_ep = bsdr_json_get_str(body, "endpoint", ep, sizeof ep);
        bsdr_mutex_lock(a->lock);
        if (on >= 0) a->browser_ctl_enabled = on != 0;
        if (have_ep && ep[0]) snprintf(a->cdp_endpoint, sizeof a->cdp_endpoint, "%s", ep);
        bsdr_mutex_unlock(a->lock);
        bsdr_app_save_settings(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/acl") == 0) {
        static char j[48 * 1024];
        int n = acl_status_json(a, j, sizeof j);
        respond(c, 200, "application/json", j, n);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/acl/requests") == 0) {
        /* on-demand: query the cloud for pending incoming friend requests (network) */
        char tok[2048]; bsdr_mutex_lock(a->lock); snprintf(tok, sizeof tok, "%s", a->bot_access_token); bsdr_mutex_unlock(a->lock);
        bsdr_friend_req reqs[32]; int nr = tok[0] ? bsdr_cloud_list_friend_requests(tok, reqs, 32) : 0;
        char j[8192]; int o = snprintf(j, sizeof j, "{\"requests\":[");
        for (int i = 0; i < nr; i++) {
            char id[160], u[128], s[160];
            bsdr_json_escape(id, sizeof id, reqs[i].notif_id);
            bsdr_json_escape(u, sizeof u, reqs[i].username);
            bsdr_json_escape(s, sizeof s, reqs[i].social_id);
            o += snprintf(j + o, sizeof j - o, "%s{\"id\":\"%s\",\"u\":\"%s\",\"s\":\"%s\"}", i ? "," : "", id, u, s);
        }
        o += snprintf(j + o, sizeof j - o, "]}");
        respond(c, 200, "application/json", j, o);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/friend") == 0) {
        char op[16] = "", u[64] = "", s[80] = "";
        bsdr_json_get_str(body, "op", op, sizeof op);
        bsdr_json_get_str(body, "username", u, sizeof u);
        bsdr_json_get_str(body, "socialId", s, sizeof s);
        if (!strcmp(op, "remove")) bsdr_acl_friend_remove(a->acl, u[0] ? u : s);
        else                       bsdr_acl_friend_add(a->acl, s[0] ? s : NULL, u);
        bsdr_app_acl_save(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/ban") == 0) {
        char key[80] = ""; bsdr_json_get_str(body, "key", key, sizeof key);   /* unban only from the UI */
        bsdr_acl_ban_remove(a->acl, key);
        bsdr_app_acl_save(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/friendreq") == 0) {
        char op[16] = "", id[128] = "", u[64] = "", s[80] = "";
        bsdr_json_get_str(body, "op", op, sizeof op);
        bsdr_json_get_str(body, "id", id, sizeof id);
        bsdr_json_get_str(body, "username", u, sizeof u);
        bsdr_json_get_str(body, "socialId", s, sizeof s);
        bool accept = !strcmp(op, "accept");
        char tok[2048]; bsdr_mutex_lock(a->lock); snprintf(tok, sizeof tok, "%s", a->bot_access_token); bsdr_mutex_unlock(a->lock);
        int code = (tok[0] && id[0]) ? bsdr_cloud_notification_action(tok, id, accept ? "accept" : "decline") : -1;
        if (accept && code / 100 == 2) { bsdr_acl_friend_add(a->acl, s[0] ? s : NULL, u); bsdr_app_acl_save(a); }
        respond(c, 200, "application/json", code / 100 == 2 ? "{\"ok\":true}" : "{\"ok\":false}", code / 100 == 2 ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/toggle") == 0) {
        char which[16] = ""; double en = 1;
        bsdr_json_get_str(body, "which", which, sizeof which);
        bsdr_json_get_double(body, "enabled", &en);
        bool on = en != 0;
        if (!strcasecmp(which, "friends")) bsdr_acl_set_friend_access(a->acl, on);
        else if (!strcasecmp(which, "hosts")) bsdr_acl_set_host_access(a->acl, on);
        else {
            uint32_t g = !strcasecmp(which,"public")?BSDR_TG_PUBLIC : !strcasecmp(which,"moderator")?BSDR_TG_MODERATOR
                       : !strcasecmp(which,"botctl")?BSDR_TG_BOTCTL : !strcasecmp(which,"computer")?BSDR_TG_COMPUTER
                       : !strcasecmp(which,"admin")?BSDR_TG_ADMIN : 0;
            if (g) bsdr_acl_set_group_enabled(a->acl, g, on);
        }
        bsdr_app_acl_save(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/websearch") == 0) {
        bsdr_mutex_lock(a->lock);
        bsdr_json_get_str(body, "endpoint", a->web_search_endpoint, sizeof a->web_search_endpoint);
        char tk[256] = ""; if (bsdr_json_get_str(body, "token", tk, sizeof tk) && tk[0]) snprintf(a->web_search_token, sizeof a->web_search_token, "%s", tk);
        bsdr_mutex_unlock(a->lock);
        bsdr_app_save_settings(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/miccheck") == 0) {
        double on = 0; bsdr_json_get_double(body, "on", &on);
        bsdr_mutex_lock(a->lock); a->mic_check_auto = on != 0; bsdr_mutex_unlock(a->lock);
        bsdr_app_save_settings(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/acl/policy") == 0) {
        /* volume policy (owner/guest gains, 0..100 %) + overload threshold */
        double go = -1, gg = -1, ov = -1;
        bsdr_json_get_double(body, "gainOwner", &go);
        bsdr_json_get_double(body, "gainGuest", &gg);
        bsdr_json_get_double(body, "overload", &ov);
        bsdr_mutex_lock(a->lock);
        if (go >= 0 && go <= 100) a->gain_owner = (float)(go / 100.0);
        if (gg >= 0 && gg <= 100) a->gain_guest = (float)(gg / 100.0);
        if (ov >= 1 && ov <= 99)  a->overload_threshold = (int)ov;
        bsdr_mutex_unlock(a->lock);
        bsdr_app_save_settings(a);
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/bot/state") == 0) {
        /* Live in-room bot state: each present speaker's resolved level + current gain, plus the
         * router's overload / mic-check / translate flags. Read-only snapshot for the status panel. */
        char j[4096]; int o = 0;
        bsdr_mutex_lock(a->lock);
        bsdr_roster s = a->roster;
        uint32_t mc = a->mic_check_ssrc;
        bool solo = a->bot_solo_owner;
        float go = a->gain_owner, gg = a->gain_guest;
        bsdr_mutex_unlock(a->lock);
        bsdr_roomcmd_status st; bsdr_roomcmd_state((bsdr_roomcmd *)a->roomcmd, &st);
        o += snprintf(j + o, sizeof j - o,
            "{\"enabled\":%s,\"queued\":%d,\"overloaded\":%s,\"micCheck\":%s,\"micUser\":\"",
            st.enabled ? "true" : "false", st.queued, st.overloaded ? "true" : "false",
            st.mic_check_active ? "true" : "false");
        char esc[128]; bsdr_json_escape(esc, sizeof esc, st.mic_check_user);
        o += snprintf(j + o, sizeof j - o, "%s\",\"translate\":%s,\"translateLang\":\"",
                      esc, st.translate_active ? "true" : "false");
        bsdr_json_escape(esc, sizeof esc, st.translate_lang);
        o += snprintf(j + o, sizeof j - o, "%s\",\"people\":[", esc);
        int emitted = 0;
        for (int i = 0; i < s.n && o < (int)sizeof j - 300; i++) {
            if (s.e[i].is_self || s.e[i].ssrc == 0) continue;
            bsdr_acl_level lvl = bsdr_acl_resolve(a->acl, s.e[i].social_id, s.e[i].username, s.e[i].is_host);
            const char *ls = lvl == BSDR_ACL_OWNER ? "owner" : lvl == BSDR_ACL_HOST ? "host" :
                             lvl == BSDR_ACL_FRIEND ? "friend" : "none";
            float g;
            if (s.e[i].ssrc == mc)               g = 1.0f;
            else if (lvl == BSDR_ACL_OWNER)      g = go;
            else if (solo)                       g = 0.0f;
            else if (lvl == BSDR_ACL_HOST || lvl == BSDR_ACL_FRIEND) g = gg;
            else                                 g = 0.0f;
            char un[128]; bsdr_json_escape(un, sizeof un, s.e[i].username);
            o += snprintf(j + o, sizeof j - o, "%s{\"name\":\"%s\",\"level\":\"%s\",\"gain\":%d,\"host\":%s}",
                          emitted ? "," : "", un, ls, (int)(g * 100 + 0.5f), s.e[i].is_host ? "true" : "false");
            emitted++;
        }
        o += snprintf(j + o, sizeof j - o, "]}");
        respond(c, 200, "application/json", j, o);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/plugstore/status") == 0) {
        char out[8192];
        size_t n = bsdr_plugstore_status_json(out, sizeof out);
        respond(c, 200, "application/json", out, n);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/plugins") == 0) {
        /* Every plugin present on disk (built-in + store-installed) with its live loaded/enabled state,
         * for the store panel's "installed on this machine" list + live enable/disable. */
        char out[8192];
        size_t n = bsdr_plugins_installed_json(out, sizeof out);
        respond(c, 200, "application/json", out, n);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/plugstore/catalog") == 0) {
        /* Relay the store catalog (can be large) — server-to-server, so no CORS issue. */
        size_t cap = 262144; char *out = malloc(cap);
        if (!out) { respond(c, 200, "application/json", "{\"ok\":false,\"error\":\"oom\"}", 26); }
        else {
            char err[256] = "";
            if (bsdr_plugstore_catalog_json(out, cap, err, sizeof err))
                respond(c, 200, "application/json", out, strlen(out));
            else {
                char ee[300]; bsdr_json_escape(ee, sizeof ee, err);
                char j[400]; int jn = snprintf(j, sizeof j, "{\"ok\":false,\"error\":\"%s\"}", ee);
                respond(c, 200, "application/json", j, jn);
            }
            free(out);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/url") == 0) {
        char url[512] = ""; bsdr_json_get_str(body, "url", url, sizeof url);
        int ok = bsdr_plugstore_set_url(url);
        respond(c, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
    } else if (strcmp(method, "POST") == 0 &&
               (strcmp(path, "/api/plugstore/login") == 0 || strcmp(path, "/api/plugstore/register") == 0)) {
        char email[256] = "", pw[256] = "";
        bsdr_json_get_str(body, "email", email, sizeof email);
        bsdr_json_get_str(body, "password", pw, sizeof pw);
        char err[256] = "";
        int ok = (strstr(path, "register") != NULL)
                     ? bsdr_plugstore_register(email, pw, err, sizeof err)
                     : bsdr_plugstore_login(email, pw, err, sizeof err);
        char ee[300]; bsdr_json_escape(ee, sizeof ee, err);
        char j[400]; int jn = snprintf(j, sizeof j, "{\"ok\":%s,\"error\":\"%s\"}", ok ? "true" : "false", ee);
        respond(c, 200, "application/json", j, jn);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/login-key") == 0) {
        char key[256] = ""; bsdr_json_get_str(body, "key", key, sizeof key);
        char err[256] = "";
        int ok = bsdr_plugstore_login_key(key, err, sizeof err);
        char ee[300]; bsdr_json_escape(ee, sizeof ee, err);
        char j[400]; int jn = snprintf(j, sizeof j, "{\"ok\":%s,\"error\":\"%s\"}", ok ? "true" : "false", ee);
        respond(c, 200, "application/json", j, jn);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/plugstore/oauth-callback", 29) == 0) {
        /* Seamless OAuth handoff: the store, after the user signs in there (password OR Google/GitHub),
         * mints a license key and redirects the browser here with ?key=. We adopt it — no manual paste.
         * The store only ever redirects to a loopback/private callback, so the key isn't exposed to a
         * public host. This lands in a browser tab, so the reply is a human page, not JSON. */
        char key[256] = "";
        const char *q = strchr(path, '?');
        if (q) { const char *k = strstr(q, "key="); if (k) url_decode(k + 4, key, sizeof key); }
        char err[256] = "";
        int ok = key[0] ? bsdr_plugstore_login_key(key, err, sizeof err) : 0;
        char he[300]; bsdr_json_escape(he, sizeof he, err[0] ? err : "no key was provided");  /* reuse the escaper for HTML-safe text */
        char html[1200];
        int hn = snprintf(html, sizeof html,
            "<!doctype html><meta charset=utf-8><title>bsdrX store sign-in</title>"
            "<body style=\"font-family:system-ui;background:#12141a;color:#e6e8ee;text-align:center;padding:12vh\">"
            "<h2>%s</h2><p>%s</p><p style=opacity:.6>You can close this tab and return to bsdrX.</p>"
            "<script>setTimeout(function(){try{window.close()}catch(e){}},2500)</script>",
            ok ? "\xE2\x9C\x94 Signed in to the plugin store"
               : "Sign-in didn\xE2\x80\x99t complete",
            ok ? "The agent is now connected to your store account." : he);
        respond(c, 200, "text/html", html, hn);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/logout") == 0) {
        bsdr_plugstore_logout();
        respond(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/buy") == 0) {
        char slug[160] = ""; bsdr_json_get_str(body, "slug", slug, sizeof slug);
        char url[700] = "";
        int ok = bsdr_plugstore_buy_url(slug, url, sizeof url);
        char ue[800]; bsdr_json_escape(ue, sizeof ue, url);
        char j[900]; int jn = snprintf(j, sizeof j, "{\"ok\":%s,\"url\":\"%s\"}", ok ? "true" : "false", ue);
        respond(c, 200, "application/json", j, jn);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/download") == 0) {
        char slug[160] = ""; bsdr_json_get_str(body, "slug", slug, sizeof slug);
        char err[256] = "";
        int ok = bsdr_plugstore_download(slug, err, sizeof err);
        char ee[300]; bsdr_json_escape(ee, sizeof ee, err);
        char j[400]; int jn = snprintf(j, sizeof j, "{\"ok\":%s,\"error\":\"%s\"}", ok ? "true" : "false", ee);
        respond(c, 200, "application/json", j, jn);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/enable") == 0) {
        char name[160] = ""; bsdr_json_get_str(body, "name", name, sizeof name);
        double on = 0; bsdr_json_get_double(body, "on", &on);
        char err[256] = "";
        int ok = bsdr_plugstore_set_enabled(name, on != 0, err, sizeof err);
        char ee[300]; bsdr_json_escape(ee, sizeof ee, err);
        char j[400]; int jn = snprintf(j, sizeof j, "{\"ok\":%s,\"error\":\"%s\"}", ok ? "true" : "false", ee);
        respond(c, 200, "application/json", j, jn);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugstore/remove") == 0) {
        char name[160] = ""; bsdr_json_get_str(body, "name", name, sizeof name);
        char err[256] = "";
        int ok = bsdr_plugstore_remove(name, err, sizeof err);
        char ee[300]; bsdr_json_escape(ee, sizeof ee, err);
        char j[400]; int jn = snprintf(j, sizeof j, "{\"ok\":%s,\"error\":\"%s\"}", ok ? "true" : "false", ee);
        respond(c, 200, "application/json", j, jn);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plugin-config") == 0) {
        /* Persist a plugin's declared config variable (only declared keys are accepted host-side). */
        char plug[160] = "", key[160] = "", val[2048] = "";
        bsdr_json_get_str(body, "plugin", plug, sizeof plug);
        bsdr_json_get_str(body, "key", key, sizeof key);
        bsdr_json_get_str(body, "value", val, sizeof val);
        int ok = bsdr_plugins_config_set(plug, key, val);
        respond(c, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
    } else if (strncmp(path, "/api/plugin/", 12) == 0 && bsdr_plugins_http(method, path, body, &c)) {
        /* a loadable plugin owned + answered this /api/plugin/<name>/... request */
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

/* Extract the bare host from a Host/Origin value into out: strip an optional scheme and any :port
 * (and [] around a v6 literal). */
static void host_component(const char *v, char *out, size_t cap) {
    const char *h = strstr(v, "://"); h = h ? h + 3 : v;
    size_t i = 0;
    if (*h == '[') { h++; while (h[i] && h[i] != ']' && i + 1 < cap) { out[i] = h[i]; i++; } }
    else { while (h[i] && h[i] != ':' && h[i] != '/' && i + 1 < cap) { out[i] = h[i]; i++; } }
    out[i] = 0;
}

/* Case-insensitive match of a Host/Origin value's host against the comma-separated allow list. */
static bool host_in_allow(struct bsdr_webui *w, const char *v) {
    if (!w->allow[0]) return false;
    char host[200]; host_component(v, host, sizeof host);
    if (!host[0]) return false;
    size_t hlen = strlen(host);
    for (const char *p = w->allow; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *s = p; while (*p && *p != ',') p++;
        size_t len = (size_t)(p - s); while (len && s[len-1] == ' ') len--;
        if (len == hlen && strncasecmp(host, s, len) == 0) return true;
    }
    return false;
}

/* CSRF / DNS-rebinding guard. A browser always sends Host, and sends Origin on cross-origin requests,
 * so we reject when either is present and NOT loopback and NOT in the operator's allow list; both
 * absent (curl, our own same-origin fetches) is allowed. Blocks a malicious page from driving the
 * panel via fetch() and blocks a rebinding domain from reaching us. With allow="*" (operator opted in,
 * e.g. behind an authenticating reverse proxy) the guard is disabled. */
static bool webui_request_local(struct bsdr_webui *w, const char *buf) {
    if (w->allow_any) return true;
    char v[256] = "";
    if (req_header(buf, "Host", v, sizeof(v)) && !host_is_local(v) && !host_in_allow(w, v)) return false;
    if (req_header(buf, "Origin", v, sizeof(v)) && !host_is_local(v) && !host_in_allow(w, v)) return false;
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
        bsdr_set_blocking(c);   /* Windows inherits the listener's non-blocking mode onto c — undo it */
        bsdr_set_recv_timeout(c, 15000);   /* a silent client can't hang this single-threaded loop */
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
                if (!webui_request_local(w, buf))
                    respond(c, 403, "application/json", "{\"error\":\"forbidden\"}", 20);
                else
                    handle(w, c, method, path, he + 4);
            }
            free(buf);
        }
        bsdr_socket_close(c);
    }
}

bsdr_webui *bsdr_webui_start(bsdr_app *app, uint16_t port, const char *bind_addr, const char *allow_hosts) {
    const char *addr = (bind_addr && bind_addr[0]) ? bind_addr : "127.0.0.1";
    struct bsdr_webui *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->app = app;
    if (allow_hosts && allow_hosts[0]) {
        snprintf(w->allow, sizeof w->allow, "%s", allow_hosts);
        if (strcmp(allow_hosts, "*") == 0) w->allow_any = 1;
    }
    w->listener = bsdr_tcp_listen(addr, port, 8);
    if (w->listener == BSDR_INVALID_SOCKET) {
        BSDR_ERROR("bsdr.webui", "listen %s:%u failed", addr, port);
        free(w); return NULL;
    }
    bsdr_set_nonblocking(w->listener);   /* so the accept loop can observe running=0 */
    w->running = 1;
    w->thread = bsdr_thread_start(loop, w);
    BSDR_INFO("bsdr.webui", "control UI at http://%s:%u", addr, port);
    if (strcmp(addr, "127.0.0.1") != 0 && strcmp(addr, "::1") != 0)
        BSDR_WARN("bsdr.webui", "control UI is NETWORK-EXPOSED on %s with no built-in auth — put it "
                  "behind a reverse proxy that adds authentication, or restrict it with a firewall.%s",
                  addr, w->allow_any ? " (CSRF guard disabled: --web-allow '*')" : "");
    return w;
}

void bsdr_webui_stop(bsdr_webui *w) {
    if (!w) return;
    w->running = 0;
    bsdr_socket_close(w->listener);
    if (w->thread) bsdr_thread_join(w->thread);
    free(w);
}
