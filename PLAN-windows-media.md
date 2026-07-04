# Plan — fully media-capable Windows agent

Goal: a Windows `bsdr_agent.exe` with the same capabilities as the Linux build —
desktop/window capture + H.264 encode, audio both directions, input injection,
LAN (`--lan`) and cloud relay paths — not just the core (signaling/DTLS) build.

## Why it's tractable
- `platform.h` already branches `_WIN32` (winsock2 + Win32 threads).
- `inject_win.c` already implements input via `SendInput`.
- The media stack is **FFmpeg-based** (`libav*`), not raw X11/NVENC — so capture
  ports by swapping the *input format*, not rewriting a capture engine.
- libsrtp2 / usrsctp / libopus / OpenSSL are all cross-compilable for mingw.

## Decisions (locked)
- **Mic-in:** target **VB-CABLE** (the Windows installer bundles + installs it);
  the agent plays the Quest mic into "CABLE Input" so apps see it as a mic.
- **FFmpeg:** **prebuilt** mingw dev build (BtbN win64-gpl-shared) — ships
  gdigrab + h264_nvenc/amf/qsv/mf/libx264 + DLLs to bundle.
- Build host: cross-compile on Linux with mingw-w64 (`x86_64-w64-mingw32`).

## What's already portable (compiles for mingw as-is)
Verified W1 compile-check against the cross-built deps:
- `video.c` (libsrtp2) — OK
- `sctp.c` (usrsctp) — OK
- `srtp_util.c` (libsrtp2) — OK
- Opus sender/receiver half of `audio.c` — OK (blocked only by the pulse include)
- All of agent / transport / control / cloud / `cloud_stream.c` — portable C.

## Platform-specific work — 3 isolated spots
1. **`capture.c` input format.** Linux uses FFmpeg `x11grab`. Add an `#ifdef _WIN32`
   branch selecting `gdigrab` (universal; `desktop` or `title=<window>`), encoder
   fallback already portable (add `h264_mf`/`h264_amf`/`h264_qsv` names). Optional
   later: `ddagrab` (DXGI, lower latency; it's an avfilter source, not an indev).
2. **`audio.c` device layer** (`bsdr_pa_*`, `bsdr_audio_player`, `bsdr_audio_devices`,
   L184–321). Split the PulseAudio IO out of `audio.c` into `audio_pulse.c` (Linux)
   vs `audio_wasapi.c` (Windows). The Opus codec half stays shared in `audio.c`.
   WASAPI: loopback capture (desktop→Quest) + render; mic-in renders to VB-CABLE.
3. **`winlist.c`** (X11 EWMH) → `winlist_win.c` via Win32 `EnumWindows` +
   `GetWindowText`/`GetWindowRect`.

## Dependencies (cross-built into one prefix by `scripts/build-win-deps.sh`)
- OpenSSL 3.0.x static — DONE
- libopus 1.5.2 static — DONE
- libsrtp2 2.6.0 static — DONE
- usrsctp static — DONE
- FFmpeg (BtbN prebuilt): `.dll.a` import libs + headers + bundled DLLs — DONE
- WASAPI/Media Foundation: native Win32, link `-lmfplat -lmfuuid -lole32 -lksuser
  -lavrt -lwinmm -lgdi32` (no external lib).

## Phases & status
- **W0 — deps:** ✅ DONE. All five cross-built/staged in `win-deps/`; recipe in
  `scripts/build-win-deps.sh` + `scripts/mingw-toolchain.cmake`.
- **W1 — link the portable stack:** ✅ DONE. `make windows-media WIN_DEPS=…` wires
  `WIN_MEDIA_SRC` + lib paths and links a full media `bsdr_agent.exe`
  (`build-windows-media/`, 7 MB) — see `ossl-build/winmedia.log`.
- **W2 — capture/encode:** ✅ code DONE — `capture.c` gdigrab `#ifdef _WIN32` branch
  + `winlist_win.c` (EnumWindows). Compiles & links. Runtime still needs a real
  Windows box/VM (gdigrab/DXGI can't run under wine) to validate.
- **W3 — audio (WASAPI):** ✅ code DONE — `audio_wasapi.c` loopback+render, mic to
  VB-CABLE; `audio.c` keeps the shared Opus codec. Compiles & links. Runtime needs
  real Windows.
- **W4 — integration + packaging:** ✅ packaging DONE for both platforms:
  - Windows: `installer/build-installer.sh` → NSIS `installer/bsdrx.nsi` →
    `installer/bsdrX-Setup-0.950.2.exe` (72 MB; bundles FFmpeg DLLs + VB-CABLE
    via `rename-mic.ps1`).
  - Linux: `packaging/build-deb.sh` → `packaging/bsdrx_0.950.2_amd64.deb`
    (dynamic media build; deps via `dpkg-shlibdeps`).
  End-to-end `--lan` + cloud *runtime* on Windows still pending real hardware.

## Validation gap (unchanged shape from LAN/cloud)
Cross-compile correctness is checkable on Linux now. Runtime capture/encode/audio
(DXGI, gdigrab, WASAPI) **cannot** be exercised under wine — W2–W4 need a real
Windows machine/VM with a GPU (+ a Quest) to validate.

## Build invocation (target, once W1 Makefile wiring lands)
```
./scripts/build-win-deps.sh            # one-time, ~15 min (FFmpeg prebuilt)
make windows-media WIN_DEPS=$PWD/win-deps
```
