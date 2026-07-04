# ANDROID.md — Android as bsdrX's 4th platform (integration handoff)

> Scope of this doc: how to add an **Android host/agent** to bsdrX **without forking a
> second project**. The Android agent does for an Android phone/tablet exactly what the
> desktop agent does for a PC: it **casts its own screen into a Bigscreen VR headset**,
> streams **audio both ways**, takes **input back** from the headset, and serves the
> local **control web UI** + **voice assistant**. Same protocol, same core, new platform
> shims. Read `../projectB-protocol-spec.md` first — the wire format is unchanged on
> Android; only the OS-facing edges differ.

## Decision summary (settled 2026-06-26)

1. **Integrate, do not fork.** Android is a 4th target *inside* bsdrX, alongside
   Linux/Windows/macOS. The protocol core is reused **verbatim**; only platform shims are
   new. Rationale: the protocol is still being pinned down live (DTLS role, SCTP de-DCEP,
   keyboard byte, mic channels, RTP PTs) — a separate repo would force every fix to land
   twice, and Android updates already add a second RE frontier. **One source of truth.**
   *Only* fork later if Android must ship as an independent product (separate repo/release/owner).
2. **Packaging: NDK core + thin Kotlin app over JNI.** The shared C core compiles to a
   native `.so` via the Android NDK; a foreground-service Kotlin app provides the
   OS-facing pieces (MediaProjection capture, MediaCodec H.264, AAudio, WebView UI) and
   talks to the core through a small JNI bridge.
3. **Privilege model: non-root (app-permission).** Capture via **MediaProjection**
   (user grants per session). Input injection via an **AccessibilityService**
   (taps/gestures/text) — *not* raw uinput. No root required; this mirrors bsdrX's
   graceful-fallback philosophy (`inject_null.c`). Full mouse/keyboard/gamepad parity is a
   **root-only** future option (see "Privilege model" §6).

## What stays the same vs. what's new

The agent's architecture is already "dumb platform shims, smart portable core." Android
slots in as another shim set. **~25 core files reused unchanged; 3 new C shims + 1 JNI
bridge + the Kotlin/Gradle app.**

| Core area | Files | Android status |
|---|---|---|
| Wire/infra | `platform.h` `net.c` `log.c` `json.c` `protocol.c` `events*` | **unchanged** (bionic is POSIX) |
| Discovery / pairing / control | `discovery.c` `control.c` | **unchanged** |
| Transport | `udp_transport.c` `dtls.c` `sctp.c` `srtp_util.c` `transport.c` | **unchanged** (NDK builds OpenSSL/usrsctp/libsrtp) |
| Input decode | `input_decode.c` | **unchanged** |
| Video RTP/SRTP | `video.c` | **unchanged** (packetizer is OS-agnostic) |
| Cloud / HTTP | `cloud.c` `cloud_stream.c` `httpc.c` | **unchanged** |
| Voice | `stt.c` `llm.c` `compcontrol.c` `voice.c` | **unchanged** (HTTP clients) |
| App / UI | `app.c` `webui.c` `overlay.c` | **unchanged** (web UI served on 127.0.0.1; reachable in-device or over LAN) |
| **Capture+encode** | `capture.c` (X11→NVENC) | **replace** → `capture_android.c` (frames in from MediaCodec via JNI) |
| **Audio I/O** | `audio.c` (PulseAudio) | **replace** → `audio_android.c` (AAudio via JNI; the Opus RTP/SRTP half of `audio.c` is portable — split it) |
| **Input inject** | `inject_linux.c` (uinput) | **replace** → `inject_android.c` (queue → AccessibilityService) |
| **Entry** | `agent.c` (CLI `main`) | **add** `bsdr_jni.c` (JNI entry; reuse agent wiring as a library) |

> Note on `audio.c`: it already separates the **portable** Opus RTP/SRTP sender/receiver
> (`bsdr_audio_sender_*`, `bsdr_audio_recv_*`) from the **PulseAudio** device half
> (`bsdr_pa_*`, `bsdr_audio_devices_*`). Keep the former; `audio_android.c` provides the
> device half against AAudio. The cleanest move is to compile the Opus half always and
> guard the `bsdr_pa_*`/`bsdr_audio_devices_*` block behind `#ifndef __ANDROID__`.

## Directory layout (added to the existing tree)

```
bsdrX/
  src/
    ...                       (existing core, untouched)
    inject_android.c          NEW  — implements include/bsdr/inject.h
    capture_android.c         NEW  — implements include/bsdr/capture.h
    audio_android.c           NEW  — implements the bsdr_pa_*/devices half of audio.h
  include/bsdr/               (unchanged; shims implement existing headers)
  android/                    NEW subtree — the Android-specific world
    settings.gradle  build.gradle  gradle.properties
    app/
      build.gradle            NDK + Gradle wiring
      src/main/
        AndroidManifest.xml
        java/net/nexlab/bsdrandroid/
          MainActivity.kt            permission flow + launches the service
          BsdrService.kt             foreground service; owns the native agent lifecycle
          NativeBridge.kt            JNI declarations (start/stop, feed frames, input cb)
          ScreenCapture.kt           MediaProjection -> Surface -> MediaCodec H.264 -> JNI
          AudioIO.kt                 AudioRecord(system/mic) + AudioTrack <-> JNI
          BsdrAccessibilityService.kt  receives input events from native, dispatches gestures/text
        cpp/
          CMakeLists.txt        globs ../../../../src core + builds the shims + bsdr_jni.c
          bsdr_jni.c            NEW  — JNI <-> agent glue (see §5)
        res/  (layouts, strings, the accessibility-service xml)
```

The NDK CMake pulls the core from `../../../../src` (relative to `app/src/main/cpp`) — the
same "compile the shared sources for another toolchain" trick the MinGW/osxcross cross
builds already use. **No core files are copied into `android/`.**

## 1. `capture_android.c` — implement `include/bsdr/capture.h`

On Android we do **not** encode in C. `ScreenCapture.kt` runs MediaProjection → a
`MediaCodec` AVC encoder configured for **Constrained Baseline** (to match what the Quest's
OpenH264 receiver expects) and hands the encoded **Annex-B access units** to native. So
`capture_android.c` is a **thread-safe ring buffer / handoff**, not an encoder:

- `bsdr_capture_open(cfg)` — record requested `out_width/out_height/fps/bitrate`; create the
  AU queue + mutex/cond. Does **not** start MediaProjection itself (the Kotlin side does,
  after user consent); it just becomes the sink JNI pushes into.
- `bsdr_capture_frame(c, &au, &len, &rtp_ts)` — pop the next AU the JNI bridge enqueued;
  block up to one frame interval; `rtp_ts` is the 90 kHz timestamp (derive from the
  MediaCodec presentation-time-us, `pts_us * 9 / 100`).
- JNI side adds `bsdr_capture_push_au(c, const uint8_t*, size_t, int64_t pts_us)` (new,
  internal) called from `bsdr_jni.c` when Kotlin delivers an encoded buffer.
- `bsdr_capture_info` / `bsdr_capture_close` — trivial.
- **Overlay:** the in-VR control bar (`overlay.c`) composites in NV12 *before* encode. On
  Android the encode happens in MediaCodec, so either (a) drop the overlay v1 and use the
  web UI only, or (b) composite onto the MediaProjection `Surface` via a `SurfaceTexture`/GL
  pass in Kotlin. **v1: web UI only; overlay deferred.**

Bitrate/FPS/resolution from the headset's `PUT /device` must reach MediaCodec: native
exposes a setter the JNI bridge surfaces up to Kotlin, which calls
`MediaCodec.setParameters()` (dynamic bitrate) and re-creates the codec on a resolution
change. Keep the **live, no-handshake** semantics the protocol requires.

## 2. `audio_android.c` — device half of `include/bsdr/audio.h`

Keep the portable Opus RTP/SRTP half of `audio.c`. Replace the PulseAudio device half:

- **Desktop-out (system audio → Quest):** Android cannot freely tap arbitrary system audio.
  Use `AudioPlaybackCapture` (API 29+, `MediaProjection`-scoped) in `AudioIO.kt` →
  interleaved `int16` PCM → JNI → `bsdr_audio_send_pcm`. Apps that opt out of capture are
  silent by policy; document this limit. There is **no "make speakers go silent"**
  equivalent — `bsdr_audio_devices_create` becomes a no-op on Android.
- **Mic-in (Quest mic → device):** `bsdr_audio_recv` decodes to PCM in the callback → JNI →
  Kotlin `AudioTrack`. Android has **no virtual microphone** without root, so the decoded
  Quest mic plays out a normal output stream (or feeds the voice-assistant STT directly).
  Wiring it back in as a system mic is **root/Magisk-only** — out of scope for v1.
- Implement `bsdr_pa_*` and `bsdr_audio_devices_*` as Android no-op/JNI-backed stubs so
  `audio.c`'s sender/receiver and the rest of the agent link unchanged.

Opus params are fixed by the protocol and unchanged: desktop **48 kHz stereo 64 kbps**,
mic **48 kHz mono 32 kbps**, 48 kHz RTP clock, AUDIO mode. (See protocol-spec §Media.)

## 3. `inject_android.c` — implement `include/bsdr/inject.h`

`input_decode.c` already turns DataChannel frames into `bsdr_input_event`s (unchanged).
`inject_android.c` is the actuator. Non-root means **no `/dev/uinput`** — instead it
**marshals events to the AccessibilityService**:

- `bsdr_injector_create(w,h)` — store screen size for abs-pointer mapping; open a queue the
  JNI bridge drains up to Kotlin (`BsdrAccessibilityService` polls/receives via a native
  callback registered from `NativeBridge`).
- `bsdr_injector_handle(inj, ev)` — translate per `events.h`:
  - **abs move** (0..65535 → pixels) → cursor position state.
  - **left down→up** → `dispatchGesture` tap at the last cursor pos; **drag** = down-move-up
    path; **wheel** → `GESTURE_SCROLL`/swipe.
  - **keyboard** (char) → focused-node `ACTION_SET_TEXT`/`commitText` via an InputMethod, or
    `performGlobalAction` for nav keys. ASCII char path matches the protocol (`VkKeyScanExA`
    equivalent is just the char itself on our side).
  - **right/middle/X1/X2/gamepad** → **no Accessibility analog**: log + drop (this is the
    documented non-root limitation; root build maps them to uinput).
- Never returns NULL; degrade to logging if the AccessibilityService isn't enabled (exact
  `inject_null.c` pattern). The app should prompt the user to enable it in Settings.

> Reality check: AccessibilityService input is **coarse** (taps/swipes/text), not a true
> pointer. It's enough to drive a touch UI from the headset, not for pixel-precise desktop
> control. That's the non-root ceiling we chose; call it out in the UI.

## 4. Kotlin app components

- **MainActivity** — request `FOREGROUND_SERVICE`/notification perms, fire the
  `MediaProjection` consent intent, prompt to enable the AccessibilityService, then start
  `BsdrService`. Shows the pairing code (from native) and a button to open the web UI.
- **BsdrService** (foreground, `mediaProjection` type) — owns the native agent: calls
  `NativeBridge.start(config)` on launch, `stop()` on teardown; holds the
  `MediaProjection`, `ScreenCapture`, `AudioIO` instances; survives app backgrounding (the
  cast must keep running with the screen off-app).
- **ScreenCapture** — `MediaProjection.createVirtualDisplay()` → input `Surface` of a
  `MediaCodec` AVC encoder (KEY_PROFILE = ConstrainedBaseline, KEY_BITRATE_MODE = CBR,
  realtime, requested W/H/fps/bitrate) → drain `outputBuffer`s on a thread →
  `NativeBridge.pushVideo(buf, ptsUs, isConfig)`. Apply dynamic bitrate via
  `setParameters`; recreate on resolution change.
- **AudioIO** — `AudioRecord` over `AudioPlaybackCaptureConfiguration` (system audio, API
  29+) → `NativeBridge.pushAudio(pcm, frames)`; `AudioTrack` fed by a native→Kotlin PCM
  callback for the Quest mic.
- **NativeBridge** — `external fun` JNI decls: `start/stop`, `pushVideo`, `pushAudio`,
  `setInputSink(svc)`, plus callbacks native invokes (`onPairingCode`, `onWantMicPcm`,
  `onInputEvent`). One `System.loadLibrary("bsdr_agent")`.
- **BsdrAccessibilityService** — registered sink for native input events; turns them into
  `dispatchGesture`/text actions (§3).

## 5. `bsdr_jni.c` — JNI ↔ agent glue

`agent.c`'s `main()` wires the whole agent together (discovery, control, transport,
capture/audio threads, web UI). Refactor its body into a callable
`bsdr_agent_run(const bsdr_agent_config*)` / `bsdr_agent_stop()` (a small, desktop-safe
change — `main` becomes a thin CLI wrapper around it). Then `bsdr_jni.c`:

- `Java_..._NativeBridge_start` — build the config (screen W/H/fps/bitrate, UI port,
  voice/STT/LLM URLs from app settings), spawn the agent on a native thread, return the
  pairing code via the `onPairingCode` callback.
- `Java_..._NativeBridge_pushVideo` — `(buf, ptsUs, isConfig)` → `bsdr_capture_push_au`.
  Cache the codec-config (SPS/PPS) AU and prepend on each IDR if MediaCodec emits it
  separately (the Quest expects in-band SPS/PPS per IDR — protocol-spec §H.264).
- `Java_..._NativeBridge_pushAudio` — PCM → `bsdr_audio_send_pcm`.
- Register the Kotlin AccessibilityService/AudioTrack as native callbacks
  (`(*env)->NewGlobalRef`; cache `JavaVM*` for `AttachCurrentThread` on the agent threads).
- `Java_..._NativeBridge_stop` — `bsdr_agent_stop`, join, release globals.

## 6. Privilege model — non-root now, root-optional later

- **Capture:** MediaProjection. Per-session user consent; a persistent notification while
  active (OS-enforced). No always-on capture without re-consent (Android 14 tightened this).
- **Input:** AccessibilityService — gestures + text only (§3). User must enable it once in
  Settings; we cannot self-grant it.
- **System-audio capture:** `AudioPlaybackCapture`, API 29+, also MediaProjection-scoped;
  per-app opt-out applies.
- **Root build (future, mirrors `inject_linux.c`):** if `/dev/uinput` is writable, prefer it
  for full mouse/keyboard/gamepad parity and add a virtual-mic via the audio HAL/loopback.
  Structure `inject_android.c` so a `uinput` path can slot in behind the same
  `bsdr_injector_handle` switch — same root-optional pattern as the desktop.

## 7. Build wiring

- **NDK deps:** OpenSSL, usrsctp, libsrtp2, libopus — cross-built for `arm64-v8a`
  (+ optionally `x86_64` for the emulator). Reuse the recipe shape in
  `scripts/build-win-deps.sh`; produce an `android/deps/<abi>/` prefix the CMake points at.
  **No ffmpeg/NVENC/X11** on Android — capture/encode is MediaCodec, so
  `BSDR_HAVE_CAPTURE`/`BSDR_ENABLE_VIDEO` for the *encoder* are off; the **RTP/SRTP video
  sender** (`video.c`) stays on. Define a build flag, e.g. `BSDR_PLATFORM_ANDROID`, and gate
  the X11/ffmpeg/PulseAudio code already behind the existing media flags.
- **CMakeLists.txt (cpp):** `add_library(bsdr_agent SHARED ...)` globbing the portable core
  from `../../../../src` (exclude `capture.c`/`audio.c` Pulse half/`inject_*`/`agent.c` CLI
  main) + the three Android shims + `bsdr_jni.c`; link the prebuilt deps + `mediandk`,
  `aaudio`, `log`, `android`.
- **Gradle:** `externalNativeBuild { cmake { path "src/main/cpp/CMakeLists.txt" } }`,
  `ndkVersion`, `abiFilters "arm64-v8a"`, `minSdk 29` (AudioPlaybackCapture), `targetSdk`
  current. Keep the desktop Make/CMake builds **untouched** — `android/` is additive.

## 8. Phased plan (mirrors the desktop phases; verify each before the next)

- **A0 — Toolchain.** NDK + Gradle skeleton; cross-build OpenSSL/usrsctp/libsrtp/opus for
  `arm64-v8a`; get the **core `.so` linking** with stub shims (no Android APIs yet).
- **A1 — Input channel end-to-end (cheapest validation).** Discovery (45000/45001) + HTTP
  pairing (45678) + DTLS/SCTP DataChannel + `input_decode` → `inject_android.c` →
  AccessibilityService. Confirm the **headset moves/taps the Android UI**. No video yet.
  (This is the desktop B-plan's "input first" step — same reason: cheap, proves transport.)
- **A2 — Video.** MediaProjection → MediaCodec CBP H.264 → `capture_android.c` → `video.c`
  RTP/SRTP → Quest renders the Android screen. Pin the **last-mile media params** live
  (PT=111 video, SSRC mapping, SPS/PPS-per-IDR) — same open items as the desktop, shared fix.
- **A3 — Audio.** AudioPlaybackCapture → Opus → Quest; Quest mic → Opus decode → AudioTrack.
- **A4 — Control UI + voice.** WebView (or system browser) on the local web UI; mirror
  bitrate/FPS/resolution knobs to MediaCodec; wire STT/LLM (already portable). Overlay
  deferred (web UI is the control surface on v1).
- **A5 — Packaging.** Foreground-service hardening (screen-off, doze), notification, consent
  re-prompt flows, signed APK. Optional `x86_64` emulator ABI for CI.
- **A6 (optional, deferred) — Root parity.** uinput injection + virtual mic behind the
  root-optional path (§6).

## 9. Open questions / risks (track alongside the desktop's live-confirm list)

- **MediaCodec ⇄ Quest codec match.** OpenH264 receiver vs Android encoder: Constrained
  Baseline, in-band SPS/PPS per IDR, no SEI/AUD spam (the desktop notes the headset caches
  SPS from pairing and dislikes SEI/AUD — verify MediaCodec output, strip if needed; the
  `bsdrx-dll-re-media-send` finding about dropping SEI/AUD applies here too).
- **AccessibilityService latency/precision** — likely fine for touch UIs, not pixel desktop
  work. Set expectations in the UI; this is the non-root ceiling.
- **System-audio capture coverage** — opted-out apps are silent; DRM audio blocked.
- **Background survival** — Android 14 foreground-service + MediaProjection lifecycle is
  strict; the service must hold the projection and re-consent cleanly.
- **Same live-confirm items as the desktop** (DTLS server cookies, SCTP de-DCEP, keyboard
  down/up byte, mic mono, mid-stream resolution change) — **shared with the core**, which is
  the whole point of integrating rather than forking: fix once, both platforms benefit.

## 10. License

Same as the rest of bsdrX: **GPLv3-or-later**, Copyright (C) 2026 Stefy Lanza
&lt;stefy@nexlab.net&gt;. New C shims carry the standard header; Kotlin/Gradle files carry the
GPLv3 short header.

## 11. Implementation status (2026-06-26)

**A0 DONE and validated** — the JNI shared library `libbsdr_agent.so` cross-compiles
and **links cleanly for `arm64-v8a`** against the NDK (26.3.11579264) with the deps
built by `scripts/build-android-deps.sh` (OpenSSL/opus/srtp2/usrsctp). The exported
`Java_net_nexlab_bsdrandroid_NativeBridge_*` entry points + `bsdr_agent_run` are present.

Landed:
- **Core refactor** — `agent.c` `main()` split into a reusable `bsdr_agent_run(opts)` /
  `bsdr_agent_stop()` (`include/bsdr/agentlib.h`); the CLI `main()` is a thin wrapper,
  excluded on Android via `BSDR_NO_CLI_MAIN`. Desktop build + tests stay green.
- **audio.c split** — the PulseAudio device half is gated behind
  `!defined(__ANDROID__)`; the Opus RTP/SRTP half + threaded player stay shared.
- **C shims** — `capture_android.c` (AU ring buffer fed by MediaCodec via JNI, SPS/PPS
  prepended per IDR, live quality republished), `audio_android.c` (AudioPlaybackCapture
  in / AudioTrack out, device-create no-op), `inject_android.c` (events → marshalled
  AccessibilityService commands), `winlist_android.c` + `filesrc_android.c` (stubs so the
  ffmpeg/X11 surface stays off).
- **JNI bridge** — `android/app/src/main/cpp/bsdr_jni.c` + `CMakeLists.txt` globbing the
  core from `../../../../../src`.
- **Kotlin app** — `MainActivity` (permission flow + MediaProjection consent),
  `BsdrService` (foreground `mediaProjection` service owning the agent + capture + audio +
  balloon), `ScreenCapture` (MediaProjection→MediaCodec CBP H.264), `AudioIO`,
  `BsdrAccessibilityService`, `NativeBridge`, plus manifest/res/Gradle.

Two product requirements added this pass (settled 2026-06-26):
- **In-app control UI, no browser.** The agent's web UI (served on `127.0.0.1:8088`) is
  embedded in a `WebView` (`MainActivity` + the balloon), and the native side starts with
  `open_browser=false`. A `network_security_config` allows cleartext to loopback only.
- **Floating "balloon" overlay.** `BubbleOverlay` (`TYPE_APPLICATION_OVERLAY`,
  `SYSTEM_ALERT_WINDOW`) is a draggable bubble that expands to the same WebView control
  panel + a Stop button, so the cast stays controllable **while navigating other apps**
  (the foreground service keeps capture alive; "Minimize to balloon" backgrounds the app).

Still pending (needs a real device + a Quest — cannot run here):
- **A1+ runtime** — discovery/pairing/DTLS input → AccessibilityService; MediaProjection →
  MediaCodec → the Quest renders the screen; AudioPlaybackCapture/mic round-trip. Same
  live-confirm items as the desktop (DTLS cookies, SCTP de-DCEP, keyboard byte, mic mono,
  SPS/PPS-per-IDR, SEI/AUD stripping) — shared with the core, fixed once for both.
- **APK packaging** signing/CI and an optional `x86_64` emulator ABI.
