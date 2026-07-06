# PLAN — parity-table caveat removal + 3rd-party dependency manager

Tracks a batch of README "Platform parity" caveats the operator asked to remove, the voice-
substitution cross-platform port, and a new web-UI dependency installer. Started 2026-07-06.

> Testability note: this dev box builds **Linux only**. No osxcross deps, no WinDivert SDK, no
> native mingw media build are present here, so the Windows/macOS code below is written to spec and
> syntax-reasoned but **must be built with `make windows` / `make osxcross` (or `./distribute.sh`)
> on a machine that has the deps** before the table cells flip to ✅/⚠️.

## A. macOS parity fixes — CODE DONE (unbuilt on target)
1. **Screen/desktop capture** `⚠️→✅`: the "grant Screen Recording" caveat is an unavoidable macOS
   TCC permission (like Android's MediaProjection, which we already score ✅). Fix = a *guided*
   one-time system prompt instead of a silent black-frame failure. `capture.c`: pure-C
   `macos_ensure_screen_capture_access()` via `CGPreflight/CGRequestScreenCaptureAccess` (CoreGraphics,
   already linked), called before the avfoundation open.
2. **Webcam source** `⚠️→✅`: was "manual index, no dropdown" because `webcam.c` is pure C and
   AVFoundation is Obj-C. Fix = `src/webcam_macos.m` (`bsdr_macos_camera_list`, returns each camera's
   localizedName as id+label — avfoundation accepts a name like the DirectShow path), wired via
   `webcam.c`'s new `__APPLE__` branch. Makefile: `.m` compile rule, `CORE_OBJ` maps `.m`, and
   `webcam_macos.m` added to `OSX_MEDIA_SRC`.
3. **Stereo-3D two-camera** `⚠️→✅`: free once #2 lands — the web UI's `camCtl` already renders a
   dropdown for BOTH eyes when enumeration returns devices. Only a stale comment was updated.

## B. Voice substitution into cloud — port off Linux/NFQUEUE
Shared `rewrite()` (decode Opus → voicefx → re-encode, keep RTP hdr/ssrc/trailer, fix IP/UDP csum) is
platform-neutral. Only the *intercept* primitive differs.
- **Windows** `❌→⚠️` (needs WinDivert + Admin): WinDivert is the exact NFQUEUE analog (inline
  capture+reinject of forwarded packets). Gate `BSDR_HAVE_WINDIVERT`. Filter string
  `"ip and udp and ip.SrcAddr == <quest>"`, `WinDivertRecv`→rewrite→`WinDivertSend`. In-process MITM
  already exists on Windows, so the flow transits us.
- **macOS** `❌→⚠️ experimental`: macOS has **no divert sockets** (ipfw removed 10.10; Apple's pf has
  no `divert-packet`). So substitution must piggyback the existing ARP-MITM: once the owner-mic flow
  is locked (dst:port:ssrc), add a scoped `pf` rule blocking exactly that flow on the egress path so
  the kernel stops forwarding the original, and reinject the rewritten datagram as an L2 frame via the
  sniffer's existing BPF inject (mc_cap). Needs micsub to receive the iface + gateway MAC + mc_cap
  handle from micsniff. Higher-risk, mark experimental.
- **Android** stays `❌` (no NFQUEUE/root/forward-intercept).

## C. Web-UI 3rd-party dependency manager  (NEW request)
Rule from operator: where **license permits AND install is automatable**, install on request from the
web UI; otherwise show a **button that opens an instructions page** with the specific steps + official
download link.

Licensing / automation matrix:
| Dependency | Platform | License | Redistribute / silent-install? | Action |
|---|---|---|---|---|
| **WinDivert** (voice-sub) | Windows | LGPLv3 **or** GPLv2 | YES — designed to ship with apps | **Bundle** in the zip (WinDivert.dll + WinDivert64.sys). No prompt. |
| **Npcap** (sniff/MITM) | Windows | proprietary (Free ed.: **no redistribution**) | NO | Instructions button → https://npcap.com (installer has `/S`, but license forbids us bundling/auto-running it). |
| **VB-CABLE** (virtual mic) | Windows | donationware, EULA redistribution needs written agreement | NO (not silently) | Instructions button → https://vb-audio.com/Cable/ |
| **ViGEmBus** (gamepad) | Windows | BSD-3-Clause | YES | Bundle/offer install (silent MSI) — optional, low priority. |
| **BlackHole** (virtual mic) | macOS | GPL-3.0-only | YES | Offer auto-install of the signed `.pkg` (needs admin) **or** instructions button. DECISION pending. |
| **ONNX Runtime + models** | all | MIT / per-model | YES | Already auto-downloaded by `model_store.c`. Surface status in the panel. |

Web surface (all in `webui.c`, served locally, same-origin):
- `GET  /api/deps` → `[{id,name,present:bool,automatable:bool,info_url,license}]` (computed per platform).
- `POST /api/deps/install {id}` → if automatable+permitted: run the installer (download to a temp dir,
  verify, execute with elevation), stream status; else return `{manual:true, info_url}`.
- `GET  /deps/<id>` → a served HTML instructions page (steps + download link) for the manual ones.
- UI: a "Dependencies" card; each row shows ✓present / Install button / "How to install" button.

## D. Build + distribution wiring (`./configure`, `make <platform>`, `./distribute.sh`)
- **Makefile**: `WIN_MEDIA_SRC` already includes `micsub.c` (via MEDIA_SRC_ALL). Add
  `BSDR_HAVE_WINDIVERT` def + `-lWinDivert` when `WIN_DEPS` has `include/windivert.h`. macOS `.m`
  wiring already added.
- **configure**: detect `windivert.h` in the win dep prefix (only relevant to `make windows`, which
  reads WIN_DEPS in the Makefile, not configure — configure is host/native). No macOS change needed
  (AVFoundation/CoreGraphics already linked).
- **scripts/build-win-bundle.sh**: copy `WinDivert.dll` + `WinDivert64.sys` from `$WIN_DEPS/bin` into
  the staged zip; extend the INSTALL note (Npcap/VB-CABLE already mentioned; add WinDivert = bundled).
- **scripts/build-osx-*.sh / osxcross image**: ensure `webcam_macos.m` compiles (frameworks present).
- **distribute.sh**: no direct change (delegates to the above scripts).
- **win-deps**: add WinDivert (dll/sys/lib/h) to the prefix; document in the win-deps memory.

## Order of execution / STATUS
1. **DONE + built** A1–A3 (macOS screen prompt, webcam .m, stereo). Verified: compiles+links on the
   real osxcross toolchain (bsdrx-osx-full image, `make osxcross OSX_HOST=o64 OSX_DEPS=/opt/ossl-x86_64`).
2. **DONE** README parity table + caveats for A and B-Windows.
3. **DONE + built** B **Windows** WinDivert backend (`micsub.c` refactored: shared platform-neutral
   `rewrite()` + NFQUEUE/WinDivert backends + stub). WinDivert 2.2.2 (LGPLv3/GPLv2) staged into
   `../win-deps` (include/windivert.h, lib/WinDivert.lib, bin/WinDivert.dll+WinDivert64.sys,
   licenses/). Makefile `WIN_WINDIVERT_DEF/LIBS` gate; `build-win-bundle.sh` copies the dll+sys+license
   + INSTALL note. `make windows WIN_DEPS=../win-deps` links WinDivert.dll (6 symbols) — VERIFIED.
   Also fixed build-blockers/warnings surfaced en route: winlist_win.c missing <stdio.h>,
   capture.c `have_raw` unused-label (non-PipeWire), micsniff.c rp_filter unused-vars (macOS).
   Docker needs `sudo` here (not in docker group); images already built.
4. **TODO** C dependency manager (endpoints + UI + instructions pages). Design in section C above.
5. **TODO** B macOS experimental piggyback (needs micsniff↔micsub plumbing; no divert socket).
6. **TODO** full `./distribute.sh` bundle run; live Quest tests (Win voice-sub, macOS screen/webcam).
