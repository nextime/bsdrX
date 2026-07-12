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

**Stop / restart is a flag flip, not a teardown** (matching the official host). Turning
sharing off keeps the sockets open and pauses transmission; video *and* audio each keep a
1 s keepalive on their relay port so the Mediasoup **comedia latch** never drops, and
turning sharing back on resumes instantly from the same ports. In `--video-decoupled`
mode the separate cloud encoder is also **released after a couple of seconds paused** (frees
the second GPU encode session while nobody is watching) and reopened on resume with a fresh
keyframe — the relay tuple stays latched throughout, so the reconnect is clean within one GOP. Source ports are **sticky
by default** — reused per relay IP across share toggles *and across a process restart*
(persisted in `~/.config/bsdr_agent/sticky_ports`), so a restarted agent rebinds the same
ports and the relay keeps forwarding. `--cloud-src-port N` hard-pins them; `--cloud-no-sticky-ports`
disables stickiness.

### Video source options

Pick the source from the web panel (or CLI). All non-desktop sources are decoded and
re-encoded so an **in-VR media bar** (play / pause, seek, volume, exit) can be
composited on top, and the source's own audio is streamed:

- **Desktop** — the full screen (default).
- **Window** — cast a single application window (all platforms: X11/Windows window
  lists, macOS CGWindowList + software crop, Android's MediaProjection app picker).
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

On a CPU-bound host, **`--faceswap-detect-every N`** runs the (dominant) face-detection
pass only every *N* frames — reusing the last boxes in between while still swapping every
frame — roughly halving faceswap CPU at the cost of slight tracking lag on fast head
motion. Default is every frame (`1`); opt-in.

### Voice changer (realtime DSP)

A no-model, low-latency DSP voice changer applied to the headset-owner's voice before
it reaches the virtual mic, computer-control, or the cloud room. A **master enable**
toggle plus knobs: **pitch** (−100…100 pitch+formant shift), **formant** (−100…100
tone/vocal-tract brightness), **volume** (±12 dB output gain), **robot** (ring-mod),
**echo**, **whisper** (breathiness) — all cross-platform (no FFT/ffmpeg) and persisted
across restarts. One-click **presets** set the sliders for you: *Feminize, Masculinize,
Younger, Older, Chipmunk, Deep, Robot, Reset*.

### AI voice (RVC voice conversion)

Behind the same changer sits a model-based **AI tier** that converts your mic to a **target
voice** (RVC) via **ONNX Runtime** — so it runs on **every platform (Linux/Windows/macOS/Android)**
with per-platform acceleration and a CPU fallback, exactly like the 2D→3D depth engine. Tick
**"use AI"** in the *AI voice (RVC)* card and pick a **Quality** tier — **CPU**, **Small GPU**, or
**Big GPU** (ONNX EPs: XNNPACK/CPU, CoreML, NNAPI, DirectML, CUDA; degrades to CPU where an
accelerator isn't present) — a **voice**, and a **pitch key** (±24 semitones). The pipeline is
ContentVec (content) + pitch (RMVPE on the GPU tiers, a built-in DSP pitch-tracker on CPU) + the
voice's RVC generator, streamed with an overlap crossfade so it keeps the same low-latency, in-place
audio contract (it adds ~one window of latency and passes audio through until warmed).

**Models are user-supplied / user-downloaded** (bsdrX ships no voice weights, so distribution stays
clean). The card manages everything: **Download engine models** (the shared ContentVec + RMVPE base,
fetched once with a progress bar) or **Import** them from a `.zip`; then **add a voice** three ways —
**Download** an RVC `.onnx` from a URL (a **find voices online ↗** link opens an RVC-ONNX model
search in a new tab; note many community voices ship as `.pth` and must be exported to ONNX first),
**Add file** from a local path (with a Browse dialog), or drop one in the library dir — each listed
with a delete button. Volume from
the DSP knobs still applies as post-gain. All state persists.

**Voice presets.** Up to **5 named custom presets** snapshot the whole changer state (AI on/tier/voice/key
plus the DSP knobs); **save** the current setup into a slot, **apply** it in one click, or **delete** it.
Persisted across restarts.

**One pipeline, two engines.** Your mic runs through a single changer that is either the **DSP
effects** or the **AI voice (RVC)** — turning on **use AI** overrides the DSP effects (only Volume
carries over). **Substitute** is the shared output routing and applies to whichever engine is on.

**Where the changed voice goes.** The changer **always** feeds the host's virtual mic
(`BSDR_QuestMic`) — so OBS/calls/computer-control hear it — but it is **not** sent to the
Quest/room unless you tick **"substitute into the cloud"** (which needs **MITM** or
**Relay** active). This now works with **either** engine — enabling only the AI voice still
substitutes into the room (previously it was gated on the DSP changer's own enable). So by default
you change your own captured voice locally without altering what the room hears. Two substitution paths:
- **Local, in-flight** (no router companion): rewrite the headset→cloud packets as they
  transit this host — **Linux** via NFQUEUE (root), **Windows** via bundled **WinDivert**
  (Administrator). **macOS-local** (experimental, wired switched LAN only): ARP-MITM +
  BPF — the privileged helper pf-drops the headset's original owner-mic and BPF-injects
  bsdrX's re-encoded copy in its place (macOS has no userland packet-divert primitive).
- **Via the relay** (all four platforms, and Android's only path): the router companion
  forwards the originals to bsdrX; bsdrX re-encodes the changed voice and hands back a
  **full modified IPv4 datagram (source = the headset)**; the companion iptables-drops the
  originals and **RAW-injects** the modified datagram so it re-uses the headset's own
  NAT/conntrack flow. That source preservation is essential: Bigscreen's mediasoup relay
  uses a **comedia latch** and rejects any packet from a different source tuple — an earlier
  build injected from a fresh companion socket (new NAT port), so the relay dropped the
  replacement and the room heard **silence**. bsdrX does all the codec/DSP; the companion
  just drops + raw-injects. **Update `bsdr_micrelay` on the router when you update bsdrX** —
  the older companion doesn't understand the full-datagram (`F`) message.

(A higher-quality RVC model tier is planned behind the same interface.)

### Microphone hijacking & capture

The Bigscreen Remote Desktop protocol has **no mic-upload channel** — the headset
owner's voice is **never sent to the PC**; it only goes to the Bigscreen *room* as
plain, unencrypted Opus RTP. bsdrX intercepts that Quest→cloud stream, decodes the
Opus, and exposes it as the virtual microphone **`BSDR_QuestMic`** (which the rest of
the OS, and the voice assistant, can use). Three capture methods:

- **Sniff** (passive) — capture the headset's traffic when this PC can already see it
  (it is the gateway, or a mirror/SPAN port). Raw packet capture, needs root/Npcap.
- **MITM** (ARP) — on a switched LAN, ARP-spoof the headset↔gateway path so the
  traffic transits this host. `--sniff-mitm`. **Best on a wired segment**: over Wi-Fi,
  ARP-spoof + hairpin forwarding is unreliable and can briefly drop the headset's LAN
  link (heartbeat timeout → the session ends). When you pick/start MITM on a Wi-Fi NIC
  the web UI warns and lets you **cancel (switch to Relay) or continue anyway**; the CLI
  logs the same warning and proceeds. Prefer **Relay** on Wi-Fi.
- **Relay** (router companion) — run **`bsdr_micrelay`** on your router: it captures
  the headset's uplink there and forwards it to the PC's `--sniff-remote PORT`. This
  is the **Wi-Fi answer** (where MITM can't work because the AP isolates stations) and
  needs **no root and no ARP** on the PC. It is also the **only** way Android gets the
  owner mic.

Two more options in the panel: **"use this computer's microphone"** (skip the headset
entirely and use the PC's own mic as the owner source) and the **cloud-room fallback**
(pull the owner's voice from the cloud SFU when no LAN capture is available).

The **capture strategy** (Sniff / MITM / Relay), the **relay port**, and the **mic on/off**
state are **persisted** (in `~/.config/bsdr_agent/settings`) and restored on the next launch —
so the owner mic comes back up automatically. Relay restores with no privilege; Sniff/MITM
re-prompt for the sudo password via the panel on restart (the password is never stored). The
**STT and LLM** endpoint, model, and token are persisted the same way — each field applies the
moment you leave it (no separate save step) and is restored on the next launch.

**Mic when you're alone — the second (bot) account.** The headset only transmits your owner
mic when there is **more than one participant** in the room (its own gate). The
**Second account (bot)** card lets you log a second Bigscreen account into bsdrX and
**Join my room** — that extra presence flips the gate so your mic streams even when no one
else is around. It's a separate session (its own token, persisted like the main one; the
password is never stored), independent of your host login.

**Presence mode (audio-only vs full bot).** The bot has two modes, selectable in the card
(and with `--bot-mode audio|full`, persisted):

- **audio only** (default) — the bot just *joins* the room over REST so `participants > 1`
  and your owner mic unlocks. It carries no media and shows **no avatar** (a userlist ghost).
  This is the original behaviour and all you need for the mic-when-alone trick.
- **full bot** — after joining, the bot also connects the room's **data plane** (raw
  usrsctp over the join's `mediaPeer` dataPort — the same unencrypted mediasoup transport the
  Quest uses, no DTLS/DCEP) and broadcasts a **UserState + periodic head-pose TickState** so the
  bot appears as a real **avatar** in the room. The wire format is reversed byte-for-byte from the
  live Quest APK and validated against a real `room.pcap`: each message is an ASCII string
  `"<legacyUserId>*base64(<4-byte type><body>)"` on SCTP stream 1 (PPID `33 00 00 00`), where
  UserState is a FlatBuffer with a **mandatory Avatar** table and TickState is a raw 176-byte pose
  struct. The prefix is the bot's room **`legacyUserId`** (`userNNN`), the exact key the Quest keys
  remote avatars by — bsdrX reads it from the room-join / `GET /room` JSON; without it the avatar
  can't render (the bot stays joined audio-only and logs a warning). Needs the SCTP media build; on
  a base build it falls back to audio-only.

**Join my room** honors your room's privacy: an **open** room is joined directly; a
**friends / verified / invite-only** room is joined via a proper **invite → accept → join**
(the host account invites the bot, the bot accepts — which stages it — then joins), with **no
privacy change**; only a **fully-closed** room is minimally raised to invite-only. An invite
may require the two accounts to be **friends** first. The RoomId is sent both with and without
its `room:` prefix (two reverse-engineerings disagree on the form) so whichever the live server
honors is used.

**Leave / Stop / Start.** Three controls sit next to Join, all keeping your host session
untouched: **Leave room** exits the current room (`GET /room/{id}/leave`, the same call the Quest
client makes) but stays logged in and online, so you can Join again; **Stop** disconnects the bot
entirely (leaves the room, drops the avatar, closes its presence WebSocket) yet **remembers the
login**, so the card then shows a one-click **Start bot** to reconnect with no password; **Log
out** (or **Forget login**) clears the saved session completely. The bot's own socialId — needed to
stage invites — is resolved the way the Quest client does it: `GET /auth/account` → `userSessionId`,
then `GET main-shark-api/info/account/userSessionId/{id}` → `socialId` (the cloud-api
`/social/profile` path is only for looking up *other* users and 403s for "me").

**Follow me into rooms.** A **Follow me** checkbox (persisted) makes the bot track you: every ~15 s
it polls your current room (`GET /rooms`) and, when you move to a different room, the bot **leaves
the old room and re-joins the new one** automatically — same privacy-honoring join path as the
manual button. Turn it off and the bot stays put. If you leave every room, the bot leaves too.

### Room mic (`BSDR_RoomMic`)

Separate from `BSDR_QuestMic` (your own voice, sniffed off the LAN), the **Room mic**
pulls the **other participants' voices** out of your Bigscreen room — the room's
mediasoup mic mix (`micPort`) — and exposes it as a virtual microphone **`BSDR_RoomMic`**
you can record into OBS, a call, etc. It also powers **computer control** when
sniff/MITM/relay aren't available. It needs an active cloud session (**Internet sharing
on** — that's the connection that carries the room). Linux exposes a dedicated device;
Windows/macOS route it into VB-CABLE/BlackHole. Enable it with the **Room mic** toggle
in the panel.

**Cloud-mic loopback (via the bot).** The SFU never sends your own voice back to you, and a producer
with no consumers can be dropped — so the room mic above carries **other** participants only and needs
someone else present. The **bot**, being a separate participant, *is* sent everyone else's audio —
**including yours**. Turn on **Cloud-mic loopback** (in the bot card, when the bot is logged in) and
the bot opens its own room-audio port, pulls that mix and feeds it into the same **`BSDR_RoomMic`** —
so your own voice reaches the computer and the room mic works **even when you're alone**, independent
of Internet sharing. When the bot owns the device the host consume defers to it (no double audio).
A **listen only to me** checkbox (on by default) solos the room owner's voice and mutes the other
participants (and the owner's desktop audio) — the room-audio receiver gained an identity-solo keyed
to `cloud_ssrc(ownerSessionId)`. Both toggles are persisted; toggling loopback starts/stops it live
if the bot is already joined.

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
- **Encoder mode** — **Quality** (default), **Balanced**, or **Performance**
  (`--encoder-mode quality|balanced|performance` or the panel dropdown, persisted).
  Higher levels use a lighter preset — Quality = NVENC `p7` + 2-pass; Balanced =
  `p6` single-pass / x264 `faster`; Performance = `p4` single-pass / x264
  `superfast` — for less CPU/GPU at a small quality cost on a constrained machine.
- **Tuning a laggy/RAM-limited host** — the desktop capture+encode runs on one thread
  that can peg a core at 1080p30; the biggest wins are usually **lower FPS** (panel
  **Max FPS** or `--fps 24`), **lower resolution** (set it on the headset), and
  **Balanced/Performance** encoder mode. Note the desktop encode is already GPU-offloaded when GPU
  is selected (capture + upload are the residual CPU cost on X11, which NVFBC-less X11 can't
  avoid — `--kmsgrab`+`--vaapi`, or the panel's **iGPU (Linux)** checkboxes, moves more of it off
  the CPU; both apply live and are persisted, kmsgrab needs `CAP_SYS_ADMIN`). For a software (`--cpu`) host that
  still can't keep up at high resolution, **x264 threads** (panel field or `--x264-threads N`)
  spreads the software encode across N cores while keeping one NAL per frame — at the cost of
  ~(N-1) frames of latency, so leave it at 1 unless you need it.
- **Wi-Fi congestion** — the panel's **Wi-Fi** controls: *send video once* (`--lan-1x`,
  halves the uplink by dropping the 2× redundancy) and **enable Wi-Fi network optimization**,
  which **DSCP/WMM-marks** the video (CS4) and audio (EF) packets so the Wi-Fi stack
  (802.11e/WMM) gives them priority airtime over background traffic — video → `AC_VI`, lifting
  the stream above downloads and other devices sharing the AP. `--max-bitrate` additionally
  caps a thin link. (DSCP marking is best-effort: honored by Linux `mac80211` and most APs;
  stock Windows ignores `IP_TOS`.)

### Privacy screen-blank, pairing, and the web panel

- **Screen-blank for privacy** — black out the *physical* monitor while the headset is
  connected (RandR brightness on Linux/X11); it restores the instant the headset
  disconnects. Desktop-only. The blank is a gamma-ramp change that outlives the process,
  so bsdrX also restores it on **Ctrl-C, `kill`, and crashes** (SIGSEGV/SIGABRT/…), and
  **self-heals on startup** — if a hard kill (SIGKILL / power loss) ever leaves the screen
  black, just relaunch bsdrX (or run **`bsdr_agent --unblank`**) to bring it back.
  (Wayland/wlroots auto-restores the moment bsdrX exits, so it never gets stuck there.)
- **Automatic LAN pairing** — zero-config discovery; the pairing code is shown at
  startup.
- **Local web control panel** — account login and status, headset picker, source
  selection, bitrate + encoder, 2D→3D, face swap, voice changer, owner-mic method,
  computer control, internet sharing, and the privacy blank — all live at
  `http://127.0.0.1:8088` (embedded WebView on Android). `--ui-port`, `--no-ui`,
  `--no-browser`. Every section except the account panel is a **collapsible panel**
  (click the header to fold it away); the open/closed state **persists** per browser,
  and each collapsed header still shows an **on/off badge** so you can see at a glance
  what's enabled. The layout is **phone-friendly** (responsive rows, larger tap
  targets) so you can drive it from a handset on the same network.

---

## Platform parity

The same C core runs on all four platforms; only the shims differ. Legend:
✅ full · ⚠️ works with a caveat · ❌ not available. Derived from the per-platform
source (`inject_*.c`, `audio*.c`, `capture*.c`, `winlist*.c`, `webcam.c`, the
encoder paths, and the build targets).

| Feature | Linux | Windows | macOS | Android |
|---|:---:|:---:|:---:|:---:|
| Screen/desktop capture → headset | ✅ Xorg (x11grab) + Wayland (portal/PipeWire) | ✅ gdigrab | ✅ avfoundation (guided Screen Recording grant) | ✅ MediaProjection |
| Window capture (single window) | ✅ | ✅ | ✅ CGWindowList + software crop | ✅ MediaProjection app picker |
| Video file / URL / playlist source | ✅ | ✅ | ✅ | ✅ (MediaExtractor transcode) |
| Webcam source | ✅ V4L2 | ✅ DirectShow | ✅ AVFoundation (enumerated dropdown) | ✅ CameraManager |
| Stereo-3D two-camera | ✅ | ✅ | ✅ | ✅ |
| Hardware (GPU) encode | ✅ NVENC/CUDA + VAAPI + kmsgrab | ✅ NVENC / AMF / QSV / MediaFoundation | ✅ VideoToolbox | ✅ MediaCodec |
| CPU encode (x264) | ✅ | ✅ | ✅ (fallback) | — (HW codec) |
| 2D→3D `fast` heuristic | ✅ | ✅ | ✅ | ✅ |
| 2D→3D built-in ONNX tiers | ✅ CUDA | ✅ DirectML | ✅ CoreML | ✅ NNAPI |
| Face swap | ✅ | ✅ | ✅ | ✅ (all need ONNX Runtime + models) |
| Voice changer (DSP) | ✅ | ✅ | ✅ | ✅ |
| AI voice (RVC, ONNX) | ✅ CUDA | ✅ DirectML | ✅ CoreML | ✅ NNAPI (all fall back to CPU) |
| Voice substitution — local (in-flight, no relay) | ✅ NFQUEUE (root) | ⚠️ WinDivert (Admin) | ⚠️ ARP-MITM+BPF (wired only, experimental) | ❌ (no local capture) |
| Voice substitution — via relay | ✅ | ✅ | ✅ | ✅ (its only method) |
| Owner-mic **Sniff** (passive) | ✅ (root helper) | ✅ bundled WinDivert (in-path) · ⚠️ Npcap (promisc/SPAN) | ✅ libpcap (built-in) · root | ❌ (no local capture) |
| Owner-mic **MITM** (ARP) | ✅ | ⚠️ Npcap + Admin (in-process) | ⚠️ | ❌ |
| Owner-mic **Relay** (`bsdr_micrelay`) | ✅ | ✅ | ✅ | ✅ (its only method) |
| Headset mic → virtual input device | ✅ PulseAudio (`BSDR_QuestMic`) | ⚠️ VB-CABLE (install) | ⚠️ BlackHole (install) | ✅ AudioTrack |
| Room mic (cloud room voice → `BSDR_RoomMic`) | ✅ PulseAudio device | ⚠️ VB-CABLE (shared) | ⚠️ BlackHole (shared) | ⚠️ MEDIA AudioTrack (capturable, not a mic) |
| Input: mouse + keyboard | ✅ uinput (+ touch mode) | ✅ SendInput (+ touch mode) | ✅ CGEvent (mouse only) | ⚠️ AccessibilityService (gestures + text; no relative mouse) |
| Input: pointer as touchpad (tap/drag touch) | ✅ uinput multitouch | ✅ InjectTouchInput | ❌ (no touch API → mouse) | — (native touch) |
| Keyboard: Unicode / international text | ✅ XTEST (X11/XWayland) | ✅ KEYEVENTF_UNICODE | ✅ CGEvent Unicode string | ✅ (soft keyboard) |
| Input: gamepad | ✅ uinput virtual XInput | ⚠️ needs ViGEmBus (guided install in panel) | ❌ (logged, unsupported) | ❌ (no Accessibility analog) |
| Computer control (voice → LLM → input) | ✅ | ✅ | ✅ | ✅ (device mic; no owner-mic gate) |
| Internet sharing (cloud relay) | ✅ | ✅ | ✅ | ✅ |
| Privacy screen-blank | ✅ (RandR/X11 + wlroots Wayland) | ✅ (gamma ramp) | ✅ (CoreGraphics gamma) | ❌ (hidden) |
| Local web control panel | ✅ browser | ✅ browser | ✅ browser | ✅ embedded WebView |
| 3rd-party dependency helper (panel) | — (deps are build requirements) | ✅ WinDivert bundled; Npcap / VB-CABLE / ViGEmBus guided | ✅ BlackHole guided | — (no external deps) |
| Automatic LAN pairing / discovery | ✅ | ✅ | ✅ | ✅ |

**Key caveats:**

- The **owner-mic sniffer needs raw packet capture** — Linux (root helper), macOS
  (libpcap is built into macOS; root for BPF). On **Windows** the passive Sniff works with
  the **bundled WinDivert** when this PC is in the headset's path (its gateway); **Npcap**
  (+ Administrator) is only needed for promiscuous/SPAN sniffing or **MITM** (ARP is L2,
  which WinDivert can't do). The panel's **Dependencies** card can fetch+launch the official
  Npcap installer on request. **Android has no local capture**, so it gets the owner mic
  **only via the router relay** (`bsdr_micrelay`).
- **MITM (ARP)** is a switched-LAN technique; over **Wi-Fi** it still works unless the AP enforces
  client isolation (bsdrX NATs the headset uplink so the AP's source-guard doesn't drop it) — if
  isolation is on, use the **Relay**.
- Cloud **voice substitution** makes the *room* hear the changed voice, two ways: **local, in-flight**
  while we MITM (Linux NFQUEUE / Windows WinDivert / macOS ARP-MITM+BPF, wired & experimental — macOS
  has no userland divert primitive), or **via the relay** on **all four platforms** (bsdrX re-encodes
  and the router companion swaps the modified audio in for the original). Android has only the relay.
  Enable it *before* joining a room (the SFU binds to the first RTP source).
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
| Privacy screen-blank | ✅ RandR | ✅ wlr-gamma-control (wlroots: sway/Hyprland/…; ❌ GNOME/KDE) |
| Root required | ❌ (only `--kmsgrab`) | ❌ (portal is user-session) |

Needs `libpipewire-0.3` + `libdbus-1` at build time (the native `./configure` and the
Linux AppImage/`.deb` bundle enable it automatically; absent → x11grab/kmsgrab only).

**`--pw-dmabuf` (experimental).** On a Wayland + Intel/AMD iGPU host, pair `--pw-dmabuf` with
`--vaapi` to negotiate a **dmabuf** from PipeWire and import the compositor's GPU surface
straight into VAAPI — zero CPU pixel movement (no map, no copy, no upload; compositor surface →
VAAPI encoder). It falls back automatically to the normal CPU PipeWire path if the
compositor/driver can't provide dmabuf or the VAAPI import fails, so it's safe to try. Off by
default; NVENC/CPU hosts are unaffected, and the voice-command balloon overlay is skipped on the
dmabuf path (no CPU surface to draw on, same as `--kmsgrab`). Needs on-hardware validation.

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
works over Wi-Fi with no ARP-spoofing and no root on the PC. It also carries **voice
substitution** on every platform: with the voice changer's "substitute into the cloud"
option on, bsdrX sends the re-encoded audio back to the relay, which drops the headset's
originals (an `iptables` FORWARD rule — needs root on the router) and forwards the changed
voice to the cloud instead. bsdrX does the codec/DSP; the relay just shuttles and swaps.

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

Media (video + audio) is **on by default**. `./configure` is the **required readiness gate**:
it detects the host OS + every needed library and runs a compile/link smoke test, failing loudly
if anything is missing (no silent feature-dropping).

### Linux (native)

```bash
./configure              # REQUIRED first: verify libs + write config.mk (re-run after distclean / dep changes)
make                     # build from config.mk (does NOT run configure) -> build/bsdr_agent
make check               # build + run the test suite
sudo make install        # bin + man pages (DESTDIR honored)
man bsdr_agent           # full option reference
```

The build is autotools-style: **`./configure && make && make install`**. `make` never runs
`./configure` itself — run configure first, and again **after `make distclean`** or whenever your
**libraries / linking change** (it re-detects deps and rewrites `config.mk`; it's idempotent, so a
no-op re-run won't force a rebuild). Bare `make` with no `config.mk` stops and tells you to configure.

CMake is also supported:
`cmake -S . -B build -DBSDR_ENABLE_VIDEO=ON -DBSDR_ENABLE_AUDIO=ON -DBSDR_ENABLE_SCTP=ON && cmake --build build -j`

**Dependencies** (Debian/Ubuntu): `libssl-dev` (always); for media add `libsrtp2-dev`,
ffmpeg (`libavcodec/avformat/avdevice/avutil/swscale-dev`), `libopus-dev`,
`libpulse-dev`, `libusrsctp-dev`, `libpcap-dev` (required — owner-mic capture). NVENC
needs the NVIDIA driver; desktop capture needs an X11 display. For the router companion:
`make micrelay` (static, bundled libpcap).

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
cd android && ./gradlew assembleRelease      # -> app/build/outputs/apk/release/app-release.apk
```

Release builds are signed: supply your own keystore via `BSDR_KEYSTORE` /
`BSDR_KEYSTORE_PASS` / `BSDR_KEY_ALIAS` / `BSDR_KEY_PASS` (or the matching `-P`
gradle properties), otherwise the build falls back to the Android **debug**
keystore so the APK is still installable (not Play-Store publishable).

Needs the Android SDK/NDK, `minSdk 29`. Casts the device screen (MediaProjection +
MediaCodec), injects input via AccessibilityService, and shows the same web UI in a
WebView. See [`ANDROID.md`](ANDROID.md) / [`docs/android.md`](docs/android.md).

### Multi-platform bundles

`./distribute.sh [linux windows osx android relay]` builds the release bundles
(see the script header for env vars); prebuilt `bsdr_micrelay` binaries for common
routers ship in `bsdrX_relay.zip`, and an OpenWRT recipe is in `openwrt/`. The
Android APK is built **release** by default (signed as above); pass `--debug` for
a debug APK.

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

## Bigscreen cloud API keys (required for cloud features)

Bigscreen cloud calls are authenticated with a Bigscreen **app key**
(`Authorization: Bearer <key>`). bsdrX uses **two**, per session role — both are
**Bigscreen's property**, so both are **removed from this repository** and stay out
**until — if ever — Bigscreen grants permission to publish them**:

| Env var | Role | Used by |
|---|---|---|
| `BSDR_CLOUD_API_KEY` | **companion** key | the host account (the RDC companion — internet-share, room mic) |
| `BSDR_CLOUD_CLIENT_KEY` | **client** key | the second/"bot" account (Friends-style client session that can join rooms) |

- **LAN remote desktop, input, audio, 2D→3D, face swap, voice, and the owner-mic
  sniffer all work with no key** — nothing to set up.
- **Cloud features** (account login, "share to internet", the second-account bot) need the key(s):
  ```bash
  export BSDR_CLOUD_API_KEY="<the Bigscreen companion key>"     # host account
  export BSDR_CLOUD_CLIENT_KEY="<the Bigscreen client key>"     # second/bot account (optional)
  ```
  `bsdr_cloud_api_key()` / `bsdr_cloud_client_key()` read these (falling back to compiled
  defaults, blank in the public build). Both keys are discoverable in Bigscreen's own
  clients (the Remote Desktop client and the Bigscreen Friends app).

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
