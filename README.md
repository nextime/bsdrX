# bsdrX — cast any screen into a Bigscreen VR headset

**bsdrX** is a clean-room **Bigscreen Remote Desktop** agent. It turns a PC — or an
Android device — into a Bigscreen Remote Desktop *host*: it casts your **screen and
audio into a Bigscreen VR headset** over the LAN (and optionally over the Internet
through the Bigscreen cloud relay), takes the headset's **mouse / keyboard / gamepad
input** back into the desktop, and adds a large set of features the stock host
doesn't have — real-time **2D→3D**, **face swap**, a **voice changer**, **owner-mic
capture**, and an LLM-driven **voice computer-control** assistant.

It is **multiplatform with feature parity as a design goal** across **Linux,
Windows, macOS and Android** — the same portable C core runs on all four, and only
the thin platform shims (capture, audio, input injection) differ. Every feature is
driven from a **local web control panel** at `http://127.0.0.1:8088` (on Android, the
very same UI is shown in an embedded WebView).

It is an **independent, clean-room implementation** of Bigscreen's Remote Desktop
wire protocol — reverse-engineered from *observed behaviour* (network captures and
black-box testing), with **no Bigscreen source code** and **no DRM circumvention**.
It pairs with a real headset on your LAN and, for internet sharing, logs into the
real Bigscreen cloud. bsdrX is not affiliated with or endorsed by Bigscreen.

## About this codebase (disclaimer)

bsdrX was built with heavy, deliberate use of **AI-assisted ("vibe") coding**: the
architecture, the protocol reverse-engineering, and every design decision were
directed and reviewed by a human developer who is fully capable of writing this code
unaided — the AI was an *accelerator*, not the author of record. This is a
protocol-interop and reverse-engineering–heavy project, and pairing a human's
judgement with AI throughput is what made its breadth (four platforms, dozens of
features) practical for one person. Everything here is **human-directed and
human-reviewed**; treat the AI's involvement as a productivity tool, exactly as you'd
treat a compiler or a code generator.

---

## Main features

### Screen / desktop casting to the headset (LAN)

The core function: capture this machine's screen, encode it to H.264, and stream it
to a paired Bigscreen headset over a single LAN UDP port (DTLS-SRTP for A/V, an SCTP
data channel for input). **Pairing is automatic** — the headset discovers the PC on
the network and a pairing code is shown at startup; every headset on the LAN is
listed so you can pick one (or pin one with `--quest_ip`). Desktop **audio streams
both ways**: the PC's sound goes to the headset (your speakers stay silent) and the
headset mic is exposed as a local virtual microphone. Just run `bsdr_agent`, put on
the headset, and pick this PC.

### Linux capture: Xorg **and** Wayland (autodetect)

Desktop capture works on **both** Linux display servers, chosen automatically:

- **Xorg** — `x11grab` (with region/window crop, `--kmsgrab` for a zero-copy DRM path).
- **Wayland** — `x11grab` can't see a native Wayland desktop (it grabs the XWayland
  root, usually blank), so bsdrX captures via **`xdg-desktop-portal` ScreenCast +
  PipeWire**: the portal shows its native picker, and the compositor's screencast is
  fed straight into the same encode pipeline. No root needed; works on GNOME, KDE and
  wlroots compositors. Built when `libpipewire-0.3` + `libdbus-1` are present.

The default **tries Xorg first and falls back to Wayland/PipeWire** on a Wayland
session; force it with **`--x11`** or **`--wayland`** (alias `--pipewire`). GPU encode
(NVENC/VAAPI) works from either source.

### Internet sharing via cloud relay

Log into your **Bigscreen account** in the web panel and toggle **"Share to
Internet"** to relay the same encoded screen + audio into a Bigscreen *room*, so
remote friends see it. It reuses the LAN encode by default (or `--video-decoupled`
for a separate encoder). Cloud features need a Bigscreen API key — see
[Bigscreen cloud API key](#bigscreen-cloud-api-key-required-for-cloud-features).

### Video source options

Pick the source from the web panel (or CLI). All non-desktop sources are decoded and
re-encoded so an **in-VR media bar** (play / pause, seek, volume, exit) can be
composited on top, and the source's own audio is streamed:

- **Desktop** — the full screen (default).
- **Window** — cast a single application window (desktop platforms with window
  enumeration).
- **Video file / playlist** — a local file, or a `.txt` **playlist** (one entry per
  line, looped). `--file PATH`.
- **http / https / rtsp URL** — stream a network video source. `--file URL`.
- **Webcam** — a single camera as the source.
- **Stereo-3D two-camera** — two cameras composited side-by-side as a *real* stereo
  pair (mount them ~6 cm apart, level); this bypasses the depth-based 2D→3D synth.

### 2D→3D — real-time depth conversion

Convert any 2D source into side-by-side stereo in real time; set your Bigscreen
screen to **SBS 3D** to view it. Three depth engines:

- **`fast`** — a light built-in depth heuristic (vertical gradient + luma) that runs
  comfortably on old laptops.
- **Built-in ONNX depth tiers** — neural monocular depth **in-process**, no Python:
  `cpu` (Depth-Anything-V2-Small, ~99 MB), `gpu` (MiDaS DPT-Hybrid, ~490 MB), `hi`
  (MiDaS DPT-Large, ~1.3 GB). GPU tiers use the platform accelerator (**CUDA /
  DirectML / CoreML / NNAPI**) and fall back to CPU. Models are **downloaded on
  demand** (with SHA-256 verification) or imported from a distributed zip for offline
  hosts.
- **External helper** — pipe frames to any external depth estimator
  (`--threed-ai CMD`); a ready-made [`scripts/bsdr-depth-helper.py`](scripts/bsdr-depth-helper.py)
  ships with a MiDaS-small model.

Tune with **deepness** (0–100) and **convergence** (−50–50); optional
**full-resolution-per-eye** mode (`--threed-full`) for a sharper, heavier frame. All
live-toggleable in the panel; applies to both the LAN and cloud streams.
Flags: `--threed off|fast|ai`, `--threed-tier cpu|gpu|hi`, `--threed-deepness`,
`--threed-convergence`, `--threed-swap`, `--threed-full`.

### Face swap (realtime deepfake)

Swap every face in the streamed video onto a **source image** you supply, in real
time. Pipeline is **insightface**-compatible ONNX run in-process: **SCRFD** detector
→ **ArcFace** (w600k) identity embedding → **inswapper_128** paste. Enable it in the
panel, point it at a face image, and pick a compute tier (CPU / GPU via
CUDA·DirectML·CoreML·NNAPI). The three `.onnx` models are non-commercial so bsdrX
never bundles them — **download on demand** into the `faceswap` model dir, or import
a zip. Applies to whatever you're streaming (desktop / webcam / file).

### Voice changer (realtime DSP)

A no-model, low-latency DSP voice changer applied to the headset-owner's voice before
it reaches the virtual mic, computer-control, or the cloud room. A **master enable**
toggle plus four knobs: **gender** (−100…100 pitch+formant shift), **robot**
(ring-mod), **echo**, **whisper** (breathiness). Cross-platform (no FFT/ffmpeg). With
**MITM** active, the optional **"substitute into the cloud"** option rewrites the
headset→cloud packets in flight — **Linux** via NFQUEUE (root), **Windows** via the
bundled **WinDivert** (Administrator) — so the *room* hears the changed voice too.
(macOS lacks a userland packet-divert primitive, so substitution is unavailable there;
a higher-quality RVC model tier is planned behind the same interface.)

### Microphone hijacking & capture

The Bigscreen Remote Desktop protocol has **no mic-upload channel** — the headset
owner's voice is **never sent to the PC**; it only goes to the Bigscreen *room* as
plain, unencrypted Opus RTP. bsdrX intercepts that Quest→cloud stream, decodes the
Opus, and exposes it as the virtual microphone **`BSDR_QuestMic`** (which the rest of
the OS, and the voice assistant, can use). Three capture methods:

- **Sniff** (passive) — capture the headset's traffic when this PC can already see it
  (it is the gateway, or a mirror/SPAN port). Raw packet capture, needs root/Npcap.
- **MITM** (ARP) — on a switched LAN, ARP-spoof the headset↔gateway path so the
  traffic transits this host. `--sniff-mitm`.
- **Relay** (router companion) — run **`bsdr_micrelay`** on your router: it captures
  the headset's uplink there and forwards it to the PC's `--sniff-remote PORT`. This
  is the **Wi-Fi answer** (where MITM can't work because the AP isolates stations) and
  needs **no root and no ARP** on the PC. It is also the **only** way Android gets the
  owner mic.

Two more options in the panel: **"use this computer's microphone"** (skip the headset
entirely and use the PC's own mic as the owner source) and the **cloud-room fallback**
(pull the owner's voice from the cloud SFU when no LAN capture is available).

### Room mic (`BSDR_RoomMic`)

Separate from `BSDR_QuestMic` (your own voice, sniffed off the LAN), the **Room mic**
pulls the **other participants' voices** out of your Bigscreen room — the room's
mediasoup mic mix (`micPort`) — and exposes it as a virtual microphone **`BSDR_RoomMic`**
you can record into OBS, a call, etc. It also powers **computer control** when
sniff/MITM/relay aren't available. It needs an active cloud session (**Internet sharing
on** — that's the connection that carries the room). Linux exposes a dedicated device;
Windows/macOS route it into VB-CABLE/BlackHole. Enable it with the **Room mic** toggle
in the panel.

Flags: `--sniff-mic`, `--sniff-mitm`, `--sniff-remote PORT`, `--sniff-iface`,
`--sniff-gw`.

### Computer control (voice → LLM → desktop)

Arm **voice-driven desktop control** and a movable **mic balloon** is drawn over the
desktop in VR. Click it, speak a request; bsdrX listens until you stop, transcribes
it (**STT**), and an **LLM** drives the desktop through a small tool set:
`type_text`, `key`, `click`, `scroll`, `open_app`. With `--compctl-vision` the model
also gets a `take_screenshot` tool for on-demand vision. STT is pluggable (any
OpenAI-compatible `/audio/transcriptions`, or a built-in keyless online service); the
LLM is any OpenAI-compatible chat endpoint — both set in the panel. It needs an
owner-mic source (sniffer, router relay, this computer's mic, or the cloud fallback).
See [`docs/computer-control.md`](docs/computer-control.md).
Flags: `--compctl`, `--compctl-vision`, `--listen-max`, `--confirm-timeout`.

### Bitrate override & encoder choice

- **Bitrate override** — the headset normally dictates the bitrate; set a value in the
  panel (or `--max-bitrate BPS`) to override it live. `0` follows the headset.
- **Encoder** — **CPU (x264)** keeps low-bitrate text crisp (best for desktops);
  **GPU** offloads the CPU and allows higher bitrate. The GPU path is NVENC/CUDA on
  Linux/Windows, VAAPI on Linux iGPUs (`--vaapi`), VideoToolbox on macOS, and
  MediaCodec on Android. `--cpu` forces full software; `--kmsgrab` is a zero-copy
  capture path on Linux with `--vaapi`.

### Privacy screen-blank, pairing, and the web panel

- **Screen-blank for privacy** — black out the *physical* monitor while the headset is
  connected (RandR brightness on Linux/X11); it restores the instant the headset
  disconnects. Desktop-only.
- **Automatic LAN pairing** — zero-config discovery; the pairing code is shown at
  startup.
- **Local web control panel** — account login and status, headset picker, source
  selection, bitrate + encoder, 2D→3D, face swap, voice changer, owner-mic method,
  computer control, internet sharing, and the privacy blank — all live at
  `http://127.0.0.1:8088` (embedded WebView on Android). `--ui-port`, `--no-ui`,
  `--no-browser`.

---

## Platform parity

The same C core runs on all four platforms; only the shims differ. Legend:
✅ full · ⚠️ works with a caveat · ❌ not available. Derived from the per-platform
source (`inject_*.c`, `audio*.c`, `capture*.c`, `winlist*.c`, `webcam.c`, the
encoder paths, and the build targets).

| Feature | Linux | Windows | macOS | Android |
|---|:---:|:---:|:---:|:---:|
| Screen/desktop capture → headset | ✅ Xorg (x11grab) + Wayland (portal/PipeWire) | ✅ gdigrab | ✅ avfoundation (guided Screen Recording grant) | ✅ MediaProjection |
| Window capture (single window) | ✅ | ✅ | ❌ (no enumeration) | ❌ (whole screen only) |
| Video file / URL / playlist source | ✅ | ✅ | ✅ | ✅ (MediaExtractor transcode) |
| Webcam source | ✅ V4L2 | ✅ DirectShow | ✅ AVFoundation (enumerated dropdown) | ✅ CameraManager |
| Stereo-3D two-camera | ✅ | ✅ | ✅ | ✅ |
| Hardware (GPU) encode | ✅ NVENC/CUDA + VAAPI + kmsgrab | ✅ NVENC / AMF / QSV / MediaFoundation | ✅ VideoToolbox | ✅ MediaCodec |
| CPU encode (x264) | ✅ | ✅ | ✅ (fallback) | — (HW codec) |
| 2D→3D `fast` heuristic | ✅ | ✅ | ✅ | ✅ |
| 2D→3D built-in ONNX tiers | ✅ CUDA | ✅ DirectML | ✅ CoreML | ✅ NNAPI |
| Face swap | ✅ | ✅ | ✅ | ✅ (all need ONNX Runtime + models) |
| Voice changer (DSP) | ✅ | ✅ | ✅ | ✅ |
| Voice substitution into cloud | ✅ NFQUEUE (root) | ⚠️ WinDivert (Admin) | ❌ (no divert socket) | ❌ |
| Owner-mic **Sniff** (passive) | ✅ (root helper) | ⚠️ Npcap + Admin | ⚠️ libpcap (root) | ❌ (no local capture) |
| Owner-mic **MITM** (ARP) | ✅ | ⚠️ Npcap + Admin (in-process) | ⚠️ | ❌ |
| Owner-mic **Relay** (`bsdr_micrelay`) | ✅ | ✅ | ✅ | ✅ (its only method) |
| Headset mic → virtual input device | ✅ PulseAudio (`BSDR_QuestMic`) | ⚠️ VB-CABLE (install) | ⚠️ BlackHole (install) | ✅ AudioTrack |
| Room mic (cloud room voice → `BSDR_RoomMic`) | ✅ PulseAudio device | ⚠️ VB-CABLE (shared) | ⚠️ BlackHole (shared) | ⚠️ MEDIA AudioTrack (capturable, not a mic) |
| Input: mouse + keyboard | ✅ uinput | ✅ SendInput | ✅ CGEvent | ⚠️ AccessibilityService (gestures + text; no relative mouse) |
| Input: gamepad | ✅ uinput virtual XInput | ⚠️ needs ViGEmBus (guided install in panel) | ❌ (logged, unsupported) | ❌ (no Accessibility analog) |
| Computer control (voice → LLM → input) | ✅ | ✅ | ✅ | ✅ (device mic; no owner-mic gate) |
| Internet sharing (cloud relay) | ✅ | ✅ | ✅ | ✅ |
| Privacy screen-blank | ✅ (RandR/X11) | ❌ | ❌ | ❌ (hidden) |
| Local web control panel | ✅ browser | ✅ browser | ✅ browser | ✅ embedded WebView |
| 3rd-party dependency helper (panel) | ✅ libpcap (detect + distro hints) | ✅ WinDivert bundled; Npcap / VB-CABLE / ViGEmBus guided | ✅ BlackHole guided | — (no external deps) |
| Automatic LAN pairing / discovery | ✅ | ✅ | ✅ | ✅ |

**Key caveats:**

- The **owner-mic sniffer needs raw packet capture** — Linux (root helper), macOS
  (libpcap, root), Windows (Npcap + Administrator). **Android has no local capture**,
  so it gets the owner mic **only via the router relay** (`bsdr_micrelay`).
- **MITM (ARP)** is a switched-LAN technique; over **Wi-Fi** it still works unless the AP enforces
  client isolation (bsdrX NATs the headset uplink so the AP's source-guard doesn't drop it) — if
  isolation is on, use the **Relay**. Cloud **voice substitution** rewrites the headset's outbound
  voice in flight while we MITM it: **Linux** via NFQUEUE (root), **Windows** via bundled **WinDivert**
  (Administrator). macOS has no userland packet-divert primitive, so it's not supported there.
- The virtual mic depends on a loopback driver you must install: **VB-CABLE** on
  Windows, **BlackHole** on macOS (PulseAudio is native on Linux). The web panel's
  **Dependencies** card lists every optional external driver/program a feature needs,
  shows whether each is already present, and links to the official installer with
  step-by-step instructions (bundling the ones whose licence allows it, e.g. WinDivert).
- **Hardware encoding matches the GPU vendor**: Windows picks NVENC / AMF (AMD) /
  QSV (Intel) / MediaFoundation, macOS uses **VideoToolbox**, Linux uses NVENC/CUDA
  or VAAPI. macOS screen capture requires the OS **Screen Recording** permission — bsdrX
  now triggers the system grant dialog on first use (a one-time approval, like Android's
  MediaProjection) instead of failing silently.
- **Android input** is capped by the AccessibilityService (gestures + text keys;
  relative mouse and gamepad are dropped). **Screen-blank** is desktop-only.

### Linux: Xorg vs Wayland

Desktop capture is autodetected per session (Xorg first, Wayland fallback; force with
`--x11` / `--wayland`). Feature support by display server:

| Capability | Xorg (X11) | Wayland |
|---|:---:|:---:|
| Whole-desktop capture | ✅ x11grab | ✅ portal ScreenCast + PipeWire |
| Zero-copy DRM capture (`--kmsgrab`) | ✅ (needs `CAP_SYS_ADMIN`) | ✅ (needs `CAP_SYS_ADMIN`) |
| Pick a single window | ✅ our window list (`_NET_CLIENT_LIST`) | ✅ via the **portal picker** (compositor dialog) |
| Region crop (x/y/w/h) | ✅ grab-time crop | ⚠️ portal picks source; crop is applied on scale |
| GPU encode (NVENC / VAAPI) | ✅ | ✅ (from the PipeWire frame) |
| Cursor in the stream | ✅ `draw_mouse` | ✅ portal `cursor_mode` (embedded) |
| Privacy screen-blank | ✅ RandR | ❌ (no RandR; compositor-specific) |
| Root required | ❌ (only `--kmsgrab`) | ❌ (portal is user-session) |

Needs `libpipewire-0.3` + `libdbus-1` at build time (the native `./configure` and the
Linux AppImage/`.deb` bundle enable it automatically; absent → x11grab/kmsgrab only).

---

## Diagrams

### Mic-hijack / capture flow

```
  Quest headset (owner speaks)
        |  plain Opus RTP
        v
  Bigscreen cloud SFU room  <-------------------------------+
        :  (intercepted off the LAN)                        |  MITM / Relay only:
        v                                                   |  rewrite cloud packets
   Capture method                                           |  in flight (NFQUEUE)
     Sniff (passive) --+                                    |
     MITM  (ARP)     --+--> Opus RTP intercept              |
     Relay (router)  --+           |                        |
                                   v                        |
                        Opus decode -> 48 kHz PCM           |
                                   |                        |
                                   v                        |
                        Voice changer DSP (optional) -------+
                                   |
              +--------------------+--------------------+
              v                                         v
   BSDR_QuestMic (virtual mic)          Computer control (STT -> LLM -> input injection)
```

### Micro-relay (`bsdr_micrelay`) over Wi-Fi

```
  Quest on Wi-Fi
        |  mic Opus RTP -> cloud (the router sees it in-path)
        v
  Router running bsdr_micrelay
        |  forwards the captured UDP
        v
  PC: bsdr_agent --sniff-remote PORT
        |
        v
  BSDR_QuestMic  +  computer control
```

`bsdr_micrelay --iface br-lan --quest <headset-ip> --to <pc-ip>:PORT` on the router;
`bsdr_agent --sniff-remote PORT` on the PC. The router is already in the path, so this
works over Wi-Fi with no ARP-spoofing and no root on the PC.

### High-level data flow

```
  Source                                   Audio
    Desktop / window  --+                    Desktop / app audio -> Opus
    Video / URL / list --+---> Processing                 |
    Webcam / stereo   --+      (2D->3D depth,             |
                               face swap,                 |
                               media-bar overlay)         |
                                    |                      |
                                    v                      |
                        H.264 encode                       |
                        (x264 / NVENC / VAAPI /            |
                         VideoToolbox / MediaCodec)        |
                                    |                      |
                                    +----------+-----------+
                                               v
                                          Transport
                          +--------------------+--------------------+
                          v                                         v
             LAN: one UDP port                          Internet: cloud relay
             (DTLS-SRTP A/V + SCTP input)                          |
                          v                                         v
                 Bigscreen headset                          Bigscreen room
```

---

## Build & run

Media (video + audio) is **on by default** and auto-detected; the build quietly drops
any feature whose libraries are missing.

### Linux (native)

```bash
./configure              # detects host OS, OpenSSL, media deps, and ONNX Runtime
make                     # -> build/bsdr_agent (full media)
make check               # build + run the test suite
sudo make install        # bin + man pages (DESTDIR honored)
man bsdr_agent           # full option reference
```

CMake is also supported:
`cmake -S . -B build -DBSDR_ENABLE_VIDEO=ON -DBSDR_ENABLE_AUDIO=ON -DBSDR_ENABLE_SCTP=ON && cmake --build build -j`

**Dependencies** (Debian/Ubuntu): `libssl-dev` (always); for media add `libsrtp2-dev`,
ffmpeg (`libavcodec/avformat/avdevice/avutil/swscale-dev`), `libopus-dev`,
`libpulse-dev`, `libusrsctp-dev`. NVENC needs the NVIDIA driver; desktop capture needs
an X11 display. For the router companion: `make micrelay` (static, bundled libpcap).

### macOS

```bash
./configure && make osx          # native build ON a Mac (CoreAudio + BlackHole)
# or cross-compile from Linux with osxcross:
make osxcross OSX_DEPS=/path/to/darwin-deps [OSX_HOST=o64|oa64]
```

Full audio + owner-mic sniffer + capture (avfoundation/VideoToolbox). Install
**BlackHole** for the virtual mic. See [`docs/macos.md`](docs/macos.md).

### Windows (MinGW-w64 cross-build)

```bash
make windows WIN_DEPS=/path/to/win-deps    # full media (alias: windows-media)
```

Capture via gdigrab, encode via NVENC/x264, virtual mic via **VB-CABLE**, owner-mic
sniffer via **Npcap**, and cloud voice **substitution** via bundled **WinDivert** —
run the agent as **Administrator** for the sniffer/MITM. `make windows` ==
`make windows-media` (full media-capable build; picks up WinDivert automatically when
`WIN_DEPS` contains the SDK).

### Android

The Android app lives in [`android/`](android/) and reuses the C core through the NDK:

```bash
scripts/build-android-deps.sh arm64-v8a     # cross-build openssl/opus/srtp2/usrsctp
cd android && ./gradlew assembleDebug        # -> app/build/outputs/apk/debug/app-debug.apk
```

Needs the Android SDK/NDK, `minSdk 29`. Casts the device screen (MediaProjection +
MediaCodec), injects input via AccessibilityService, and shows the same web UI in a
WebView. See [`ANDROID.md`](ANDROID.md) / [`docs/android.md`](docs/android.md).

### Multi-platform bundles

`./distribute.sh [linux windows osx android relay]` builds the release bundles
(see the script header for env vars); prebuilt `bsdr_micrelay` binaries for common
routers ship in `bsdrX_relay.zip`, and an OpenWRT recipe is in `openwrt/`.

### Common run examples

```bash
./build/bsdr_agent                          # cast the desktop; panel at :8088
./build/bsdr_agent --sniff-mitm             # + intercept the owner mic on a switched LAN
./build/bsdr_agent --sniff-mic --compctl-vision   # voice computer-control with vision
./build/bsdr_agent --file movie.mp4 --threed fast # a 2D→3D movie
./build/bsdr_agent --threed-tier cpu --threed ai  # built-in neural depth
```

See `man bsdr_agent` / [`docs/bsdr_agent.1`](docs/bsdr_agent.1) for every option.

### Linux `/dev/uinput` permissions

`/dev/uinput` is root-only by default; to inject as your user:

```bash
sudo groupadd -f uinput
echo 'KERNEL=="uinput", GROUP="uinput", MODE="0660"' | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload && sudo modprobe uinput
sudo usermod -aG uinput "$USER"   # re-login
```

Without it the agent still runs — injection just falls back to logging.

---

## Bigscreen cloud API key (required for cloud features)

Every Bigscreen cloud call is authenticated with Bigscreen's **client API key**
(`Authorization: Bearer <key>`). That key is **Bigscreen's property**, so it has been
**removed from this repository** and stays out **until — if ever — Bigscreen grants
permission to publish it**.

- **LAN remote desktop, input, audio, 2D→3D, face swap, voice, and the owner-mic
  sniffer all work with no key** — nothing to set up.
- **Cloud features** (account login, "share to internet") need you to supply the key:
  ```bash
  export BSDR_CLOUD_API_KEY="<the Bigscreen client key>"
  ```
  `bsdr_cloud_api_key()` reads that variable (falling back to a compiled default, blank
  in the public build). The key is discoverable in Bigscreen's own Remote Desktop
  client config.

---

## Interoperability & clean-room note

bsdrX is an **independent, clean-room implementation**: it speaks Bigscreen's Remote
Desktop protocol so it can interoperate with the stock app and headset, but it
contains **no Bigscreen source code**. The protocol was learned from **observed
behaviour** (network captures and black-box testing); nothing is decompiled-and-pasted,
and it **circumvents no DRM or copy protection**. After a Bigscreen update, protocol
details (IL2CPP names/offsets) can shift — re-run the protocol dump if a stream stops
rendering. "Bigscreen" is a trademark of its owner, used here only to describe
compatibility.

## Status

The desktop, audio, and input paths, the web panel, 2D→3D, face swap, voice changer,
owner-mic capture, computer control, and cloud sharing are in day-to-day use on Linux;
Windows, macOS, and Android build and run the shared core with the platform caveats in
the parity table above. On-headset rendering is confirmed against a real Quest.

## License

bsdrX is free software under the **GNU General Public License v3.0 (or later)** —
Copyright (C) 2026 **Stefy Lanza &lt;stefy@nexlab.net&gt;**. It comes with **no
warranty**; the full text is in [LICENSE.md](LICENSE.md). Project page:
[bigscreen.nexlab.net](https://bigscreen.nexlab.net).
</content>
</invoke>
