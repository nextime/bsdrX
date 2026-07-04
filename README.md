# bsdrX — cast your Linux desktop into a Bigscreen VR headset

bsdrX turns a PC into a Bigscreen **Remote Desktop host**. It streams your
**desktop — or a video file — into a Bigscreen VR headset** over the LAN, carries
**audio both ways**, takes **mouse / keyboard / gamepad input back** from the
headset, and adds things the stock host doesn't: **real-time 2D→3D**, an **in-VR
media bar**, and a **voice assistant** that can type and drive the desktop through
an LLM. It's portable C — full-featured on **Linux**, with **Windows** and
**macOS** builds too.

It's an independent, clean-room implementation that speaks Bigscreen's Remote
Desktop protocol — implemented from observed behaviour, with no official code and
no DRM circumvention. It pairs with the real headset on your LAN and logs into the
real Bigscreen cloud for "share to internet."

## Features

**Desktop → headset (LAN)**
- **Automatic pairing** — the headset finds this PC on the network; a pairing code
  is shown at startup. Every headset on the LAN is listed so you can pick one.
- **Video**: X11 desktop capture → **NVENC** H.264 (fallback libx264) → the headset.
  Or **stream a video file, an http/https/rtsp URL, or a `.txt` playlist**,
  re-encoded with an **in-VR media bar** (play/pause, seek, volume) and its own audio.
- **Real-time 2D→3D** (side-by-side): a light built-in depth heuristic (**fast**,
  fine on old laptops) or an external depth model (**ai**, e.g.
  [`scripts/bsdr-depth-helper.py`](scripts/bsdr-depth-helper.py) with MiDaS), with
  **deepness / convergence** calibration and an optional **full-resolution-per-eye**
  mode. Set your Bigscreen screen to SBS 3D to view it. Live-toggleable in the web
  panel; applies to both the LAN and cloud streams.
- **Audio both ways**: desktop sound → the headset (your speakers stay silent), and
  the headset mic → a **virtual microphone** the rest of the OS can use as an input.
- **Input back**: mouse (absolute/relative), all buttons, wheel, keyboard and XInput
  gamepad, injected into the desktop via **uinput** (Linux) / **SendInput** (Windows)
  / **CGEvent** (macOS).
- **Privacy screen-blank**: black out the physical monitor while the headset is
  connected; it restores the instant the headset disconnects.

**Share to internet (cloud)**
- Log into your **Bigscreen account** and relay the same desktop/video + audio into
  a Bigscreen room, so remote friends see it — toggled from the web panel.

**Local web control panel** (`http://127.0.0.1:8088`, opens in your browser)
- Account login and connection status, headset picker, source (desktop / file / URL
  / playlist), **resolution + bitrate**, 2D→3D controls, owner-mic options, internet
  sharing, and the privacy screen-blank — all live.

**Voice computer control** (optional; enable in the web panel)
- A movable **mic balloon** is drawn over the desktop in VR. Click it, speak a
  request; it listens until you stop, transcribes it, and an **LLM drives the
  desktop** with a small tool set (`type_text`, `key`, `click`, `scroll`, `open_app`).
- **Speech-to-text** is pluggable — point it at a local/remote whisper server (any
  OpenAI-compatible endpoint) or use the built-in keyless online service. The **LLM**
  is any OpenAI-compatible chat endpoint. See
  [`docs/computer-control.md`](docs/computer-control.md).

## Build

Media (video / audio) is **on by default** and auto-detected — `make` builds the
full agent on Linux and quietly drops any feature whose libraries are missing.

```bash
./configure              # detects host OS, OpenSSL, and media deps
make                     # -> build/bsdr_agent
make check               # build + run the test suite
sudo make install        # -> $(prefix) (bin, man page; DESTDIR honored)
man bsdr_agent           # full option reference

./configure --disable-media     # input-only agent; also --disable-video/-audio/-sctp
make linux-static               # static Linux binary (core agent, no media)

# CMake is supported too:
cmake -S . -B build -DBSDR_ENABLE_VIDEO=ON -DBSDR_ENABLE_AUDIO=ON && cmake --build build -j
```

**Dependencies** (Debian/Ubuntu names): `libssl-dev` (always); for media add
`libsrtp2-dev`, ffmpeg (`libavcodec/avformat/avdevice/avutil/swscale-dev`),
`libopus-dev`, `libpulse-dev`, and `libusrsctp-dev`. NVENC needs the NVIDIA driver;
desktop capture needs an X11 display.

- **Windows** (MinGW-w64 cross-build) and **macOS** (native, with CoreAudio +
  BlackHole audio) are supported; see [`docs/macos.md`](docs/macos.md) for the mac
  setup. On these platforms the input + transport + audio paths are built (NVENC /
  X11 capture are Linux-only).

### Linux uinput permissions
`/dev/uinput` is root-only by default; to let the agent inject as your user:
```bash
sudo groupadd -f uinput
echo 'KERNEL=="uinput", GROUP="uinput", MODE="0660"' | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload && sudo modprobe uinput
sudo usermod -aG uinput "$USER"   # re-login
```
Without it the agent still runs — injection just falls back to logging.

## Run

```bash
./build/bsdr_agent
#   --file PATH                stream a video file / http|https|rtsp URL / .txt playlist
#   --threed off|fast|ai       real-time 2D->3D side-by-side
#   --threed-deepness N        depth amount 0..100     (--threed-convergence -50..50)
#   --threed-full              full resolution per eye (heavy; default light half-SBS)
#   --threed-ai CMD            external depth helper for --threed ai
#   --no-video / --no-audio    disable a media direction
#   --fps N --bitrate BPS
#   --cpu                      full software encode path (no GPU)
#   --ui-port N / --no-ui / --no-browser
```
The agent prints a **pairing code** and opens the **control panel** at
`http://127.0.0.1:8088`. Put on the headset, pick this PC, and start the session —
input, desktop video, and audio come up together. Audio devices are only created
on a real connection, so the idle agent never touches your sound.

## How it works

```
src/
  discovery control        headset discovery (UDP) + pairing/control (HTTP)
  udp_transport dtls sctp   one UDP port -> DTLS-SRTP media + SCTP data channel
  input_decode inject_*     headset input -> native injection (uinput/SendInput/CGEvent)
  capture video srtp_util   X11 + NVENC/x264 -> RTP/SRTP H.264; file/URL source
  threed                    2D->3D side-by-side (fast heuristic or AI depth)
  audio                     Opus RTP/SRTP + PulseAudio; virtual mic/sink
  overlay                   in-VR media bar + voice balloon (composited on the video)
  cloud cloud_stream httpc  Bigscreen account login + internet-share relay
  stt llm compcontrol voice speech-to-text + LLM tool-calling + desktop control
  app webui agent           shared state, local web panel, and the wiring
```

Everything runs on one machine; the in-headset side is stock Bigscreen. The LAN
media path is a single UDP port (DTLS-SRTP for A/V, an SCTP data channel for input);
the cloud path relays the same encoded stream into a Bigscreen room.

## Interoperability & clean-room note

bsdrX is an **independent, clean-room implementation** — it speaks Bigscreen's
Remote Desktop protocol so it can interoperate with the stock app and headset, but
it contains **no Bigscreen source code**. The protocol was learned from **observed
behaviour** (network captures and black-box testing of the shipping app); nothing is
decompiled-and-pasted, and it **circumvents no DRM or copy protection**. bsdrX is not
affiliated with, endorsed by, or supported by Bigscreen. "Bigscreen" is a trademark
of its owner and is used here only to describe compatibility.

## Bigscreen cloud API key (required for the cloud features)

Every Bigscreen cloud call is authenticated with Bigscreen's **client API key**
(`Authorization: Bearer <key>`). That key is **Bigscreen's property**, so it has been
**removed from this repository** and will stay out **until — if ever — Bigscreen
grants permission to publish it**. As a result:

- **LAN remote desktop, input, audio, and 2D→3D work with no key** — nothing to set up.
- **Cloud features** (account login, "share to internet") need you to supply the key
  yourself: export it before launching —
  ```bash
  export BSDR_CLOUD_API_KEY="<the Bigscreen client key>"
  ```
  `bsdr_cloud_api_key()` reads that environment variable (falling back to a compiled
  default, which is blank in the public build). The key is discoverable in the config
  of Bigscreen's own Remote Desktop client.

## Status

The desktop, audio and input paths, the web panel, 2D→3D, and cloud sharing are all
in use day-to-day on Linux. Windows and macOS build and run the input/transport/audio
paths; on-headset rendering is confirmed against a real Quest. After a Bigscreen
update, media details can shift — see the docs if a stream stops rendering.

## License

bsdrX is free software under the **GNU General Public License v3.0 (or later)** —
Copyright (C) 2026 **Stefy Lanza &lt;stefy@nexlab.net&gt;**. It comes with **no
warranty**; the full text is in [LICENSE.md](LICENSE.md).
