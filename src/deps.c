/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* deps.c — optional 3rd-party dependency manager. See deps.h.
 *
 * Per-platform tables of the external programs/drivers bsdrX features can need, with conservative
 * presence detection and a universal fallback: a per-dependency instructions page carrying the steps
 * and the OFFICIAL download link. Deps we bundle (WinDivert, dual LGPLv3/GPLv2) report as present with
 * nothing to install; deps whose license forbids redistribution (Npcap, VB-CABLE) or that are kernel
 * drivers requiring user consent are manual — the UI shows a "How to install" button to /deps/<id>.
 */
#include "bsdr/deps.h"
#include "bsdr/log.h"

#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

/* ---- detection helpers (return 1 only when positively confirmed) ---- */

#if defined(_WIN32)
/* A runtime DLL is present if it loads. FreeLibrary right after — we only probe availability. */
static int win_dll_present(const char *dll) {
    HMODULE h = LoadLibraryA(dll);
    if (!h) return 0;
    FreeLibrary(h);
    return 1;
}
/* VB-CABLE (or our renamed "BSRD_Mic") shows up as a waveOut/waveIn endpoint whose product name
 * contains "CABLE" or "BSRD". waveOut*GetDevCaps is in winmm (already linked). */
static int win_vbcable_present(void) {
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEOUTCAPSA caps;
        if (waveOutGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR &&
            (strstr(caps.szPname, "CABLE") || strstr(caps.szPname, "BSRD") || strstr(caps.szPname, "BSDR")))
            return 1;
    }
    n = waveInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR &&
            (strstr(caps.szPname, "CABLE") || strstr(caps.szPname, "BSRD") || strstr(caps.szPname, "BSDR")))
            return 1;
    }
    return 0;
}
#endif

/* ---- per-platform dependency tables ----
 * Fields: id, name, purpose, license, info_url, present(filled below), bundled, automatable. */

int bsdr_deps_list(bsdr_dep *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
#define ADD(ID,NAME,PURPOSE,LIC,URL,BUNDLED,AUTO) \
    do { if (n < max) { out[n].id=ID; out[n].name=NAME; out[n].purpose=PURPOSE; out[n].license=LIC; \
         out[n].info_url=URL; out[n].present=0; out[n].bundled=(BUNDLED); out[n].automatable=(AUTO); n++; } } while (0)

#if defined(_WIN32)
    ADD("windivert", "WinDivert", "Owner-mic cloud voice substitution (rewrites your voice to the room)",
        "LGPLv3 / GPLv2 (bundled)", "https://reqrypt.org/windivert.html", 1, 0);
    ADD("npcap", "Npcap", "Owner-mic Sniff / MITM packet capture",
        "Free edition (proprietary, not redistributable)", "https://npcap.com", 0, 0);
    ADD("vbcable", "VB-CABLE", "Virtual microphone: headset mic + voice computer-control",
        "Donationware (installer only)", "https://vb-audio.com/Cable/", 0, 0);
    ADD("vigembus", "ViGEmBus", "Virtual gamepad (XInput) input injection",
        "BSD-3-Clause", "https://github.com/nefarius/ViGEmBus/releases/latest", 0, 0);
    /* presence */
    for (int i = 0; i < n; i++) {
        if (!strcmp(out[i].id, "windivert")) out[i].present = win_dll_present("WinDivert.dll");
        else if (!strcmp(out[i].id, "npcap")) out[i].present = win_dll_present("wpcap.dll");
        else if (!strcmp(out[i].id, "vbcable")) out[i].present = win_vbcable_present();
        /* vigembus: no cheap probe — leave 0 ("not detected") */
    }

#elif defined(__APPLE__)
    ADD("blackhole", "BlackHole", "Virtual microphone: headset mic + cloud room mic",
        "GPL-3.0", "https://existential.audio/blackhole/", 0, 0);
    /* libpcap ships with macOS; the Sniff/MITM path just needs root (no separate install). */

#elif defined(__linux__) && !defined(__ANDROID__)
    /* On Linux the native install (.deb / AppImage) pulls these via the package manager; listed so the
     * panel can flag a missing optional lib. PulseAudio/PipeWire is the native virtual-audio backend
     * (no external driver). */
    ADD("libpcap", "libpcap", "Owner-mic Sniff / MITM packet capture",
        "BSD (distro package)", "https://www.tcpdump.org/", 0, 0);
#if defined(BSDR_HAVE_PCAP)
    for (int i = 0; i < n; i++) if (!strcmp(out[i].id, "libpcap")) out[i].present = 1;
#endif
#endif

#undef ADD
    return n;
}

int bsdr_dep_install(const char *id, char *msg, int msgcap) {
    if (!id || !msg || msgcap <= 0) return -1;
    bsdr_dep deps[16];
    int n = bsdr_deps_list(deps, 16);
    for (int i = 0; i < n; i++) {
        if (strcmp(deps[i].id, id) != 0) continue;
        if (deps[i].bundled) {
            if (deps[i].present) { snprintf(msg, msgcap, "%s is bundled with bsdrX and is present.", deps[i].name); return 1; }
            snprintf(msg, msgcap, "%s ships with bsdrX — make sure its files sit next to the executable.", deps[i].name);
            return 0;
        }
        if (deps[i].present) { snprintf(msg, msgcap, "%s is already installed.", deps[i].name); return 1; }
        /* Not bundled, not present, and (in this build) not auto-installable: point the operator at the
         * step-by-step instructions page, which carries the official download link. Auto-install per
         * dep can be added here later (fetch + launch the official installer) where the license allows. */
        snprintf(msg, msgcap, "%s must be installed manually — open the instructions for the download link and steps.", deps[i].name);
        return 0;
    }
    snprintf(msg, msgcap, "unknown dependency \"%s\"", id);
    return -1;
}

/* ---- instructions pages (HTML fragments served at /deps/<id>) ---- */
const char *bsdr_dep_page(const char *id) {
    if (!id) return NULL;
    if (!strcmp(id, "windivert"))
        return "<h1>WinDivert</h1>"
               "<p><b>Bundled — no action needed.</b> WinDivert powers owner-mic cloud <b>voice "
               "substitution</b> (making the Bigscreen room hear your changed voice while you MITM the "
               "headset). bsdrX ships <code>WinDivert.dll</code> and <code>WinDivert64.sys</code> next to "
               "<code>bsdr_agent.exe</code>; keep them there. It loads on first use and needs bsdrX to run "
               "<b>as Administrator</b>.</p>"
               "<p>License: dual LGPLv3 / GPLv2. Project: "
               "<a href=\"https://reqrypt.org/windivert.html\" target=_blank>reqrypt.org/windivert.html</a></p>";
    if (!strcmp(id, "npcap"))
        return "<h1>Npcap</h1>"
               "<p>Npcap provides the raw packet capture the owner-mic <b>Sniff</b> and <b>MITM</b> methods "
               "need. Its licence does not permit us to redistribute or silently install it, so install it "
               "yourself once:</p>"
               "<ol><li>Download the installer from "
               "<a href=\"https://npcap.com\" target=_blank>https://npcap.com</a>.</li>"
               "<li>Run it and tick <b>&quot;Install Npcap in WinPcap API-compatible Mode&quot;</b>.</li>"
               "<li>Restart bsdrX <b>as Administrator</b>.</li></ol>"
               "<p>If you only use the <b>Relay</b> method (router companion), you don't need Npcap.</p>";
    if (!strcmp(id, "vbcable"))
        return "<h1>VB-CABLE</h1>"
               "<p>VB-CABLE is the virtual audio device bsdrX routes the headset microphone (and voice "
               "computer-control) into, so other apps see a &quot;BSRD_Mic&quot; microphone. Donationware; "
               "we can't bundle it, so install the official driver:</p>"
               "<ol><li>Download from "
               "<a href=\"https://vb-audio.com/Cable/\" target=_blank>https://vb-audio.com/Cable/</a>.</li>"
               "<li>Unzip and run <b>VBCABLE_Setup_x64.exe as Administrator</b>, click <b>Install Driver</b>.</li>"
               "<li>Reboot if prompted, then restart bsdrX.</li></ol>";
    if (!strcmp(id, "vigembus"))
        return "<h1>ViGEmBus</h1>"
               "<p>ViGEmBus is the virtual-gamepad driver needed to inject <b>XInput gamepad</b> events "
               "(mouse/keyboard work without it). BSD-3-Clause:</p>"
               "<ol><li>Download the latest installer from "
               "<a href=\"https://github.com/nefarius/ViGEmBus/releases/latest\" target=_blank>the ViGEmBus releases page</a>.</li>"
               "<li>Run it (it's a signed MSI), accept the driver install.</li>"
               "<li>Restart bsdrX.</li></ol>";
    if (!strcmp(id, "blackhole"))
        return "<h1>BlackHole</h1>"
               "<p>BlackHole is the virtual audio driver bsdrX routes the headset microphone and the cloud "
               "room mic into on macOS. GPL-3.0:</p>"
               "<ol><li>Download the installer from "
               "<a href=\"https://existential.audio/blackhole/\" target=_blank>https://existential.audio/blackhole/</a> "
               "(the 2-channel edition is enough).</li>"
               "<li>Run the <b>.pkg</b> and complete the install (needs your admin password).</li>"
               "<li>Restart bsdrX.</li></ol>";
    if (!strcmp(id, "libpcap"))
        return "<h1>libpcap</h1>"
               "<p>libpcap is the packet-capture library the owner-mic <b>Sniff</b> / <b>MITM</b> methods "
               "use. The <code>.deb</code> and AppImage bundle it; if you built from source, install it "
               "with your package manager:</p>"
               "<ul><li>Debian/Ubuntu: <code>sudo apt install libpcap0.8</code></li>"
               "<li>Fedora: <code>sudo dnf install libpcap</code></li>"
               "<li>Arch: <code>sudo pacman -S libpcap</code></li></ul>"
               "<p>Then restart bsdrX. (The <b>Relay</b> method needs no local capture.)</p>";
    return NULL;
}
