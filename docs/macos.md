# bsdrX on macOS

macOS builds get the **full audio feature set** — desktop-audio → Quest, Quest/room mic → a virtual
microphone, and the **owner-mic sniffer** (intercept the headset owner's voice off the LAN). The
audio DSP is shared with Linux/Windows; macOS supplies the I/O via **CoreAudio**, and — because
macOS has no built-in loopback — a user-installed virtual audio driver, **BlackHole**.

> Status: the macOS backend (`src/audio_coreaudio.c`) is written to the documented CoreAudio APIs
> but has **not yet been compiled/run on a Mac in CI**. Treat the first build as a bring-up. Please
> report issues.

---

## 1. Prerequisites

Install [Homebrew](https://brew.sh), then the build + runtime deps:

```sh
brew install opus libsrtp pkg-config      # audio codec + SRTP + pkg-config
brew install blackhole-2ch                # virtual audio driver (the "loopback")
```

- **libpcap** ships with macOS (used for packet capture) — no install needed.
- **BlackHole** is the macOS equivalent of VB-CABLE: a loopback audio device. bsdrX renders the
  decoded voice into it, and other apps record from it as a microphone. Homepage:
  <https://existential.audio/blackhole/>.

After installing BlackHole you may need to log out/in (or reboot) for the driver to load. Verify:

```sh
system_profiler SPAudioDataType | grep -i blackhole
```

---

## 2. Build

```sh
./configure          # auto-detects Darwin -> macOS target, CoreAudio + BlackHole + libpcap
make
# binary: build/bsdr_agent
```

`./configure` prints the audio line it chose, e.g.:

```
configure: macOS full audio = audio.c (Opus DSP) + CoreAudio/BlackHole + libpcap sniffer
```

If a dep is missing, `pkg-config` will fail — install the brew package above and re-run `./configure`.

---

## 3. Audio routing with BlackHole

macOS can't capture system output or expose a virtual mic without a loopback, so everything routes
through BlackHole. The two directions:

- **Desktop audio → Quest**: set the system output to BlackHole so bsdrX can capture it. To still
  hear the audio yourself, create a **Multi-Output Device** (below) that plays to *both* your
  speakers and BlackHole, and select that as the system output.
- **Quest / owner mic → apps**: bsdrX renders the decoded voice into BlackHole; any app (Zoom,
  Discord, a recorder, the moderator bot) then selects **BlackHole** as its microphone input.

### Create a Multi-Output Device (so you still hear desktop audio)

1. Open **Audio MIDI Setup** (`/Applications/Utilities/Audio MIDI Setup.app`).
2. Click **+** (bottom-left) → **Create Multi-Output Device**.
3. Tick both your **Built-in Output** (or headphones) **and BlackHole 2ch**.
4. Set that Multi-Output Device as the system output (System Settings → Sound → Output).

Now you hear audio normally *and* bsdrX can capture it from BlackHole.

### The single-loopback caveat

A stock **BlackHole 2ch** is *one* device. bsdrX uses BlackHole for several things — desktop-audio
capture, the room mic, and the owner-mic sniffer — so with a single 2ch device those paths share one
loopback and will mix. For **independent** streams at the same time (e.g. desktop audio out *and* a
clean owner-mic in), install a second loopback and point one path at it:

```sh
brew install blackhole-16ch     # a second, distinct loopback device
```

Device names are matched by substring, so a device containing "BlackHole" is picked automatically;
with two installed you can dedicate one to the mic and one to desktop audio.

---

## 4. Owner-mic sniffer

The Bigscreen remote-desktop protocol never sends the headset owner's mic to the PC — that voice
only goes to the Bigscreen room (the mediasoup cloud) as plain Opus RTP. The sniffer intercepts it
off the LAN and exposes it as the **BSDR-Quest-OwnerMic** input (via BlackHole).

```sh
# passive capture (needs this Mac to see the headset's traffic — be the gateway or a mirror port)
sudo ./build/bsdr_agent --sniff-mic --quest_ip 192.168.x.y --sniff-iface en0

# switched LAN: reroute the headset's traffic through this Mac (ARP MITM)
sudo ./build/bsdr_agent --sniff-mitm --quest_ip 192.168.x.y --sniff-iface en0 --sniff-gw 192.168.x.1
```

- Packet capture needs root. Run the agent normally as your user and it will re-exec a small
  **helper via `sudo`** for the capture only (it prompts on the terminal); or start the whole agent
  with `sudo`. From the **web panel** (`http://127.0.0.1:8088`) the "Headset owner mic" card has a
  password field that feeds `sudo -S`.
- `--sniff-iface` is usually `en0` (Wi-Fi/Ethernet). List interfaces with `ifconfig` or
  `networksetup -listallhardwareports`.
- MITM uses `sysctl net.inet.ip.forwarding` / `net.inet.ip.redirect` (restored on exit) and ARP via
  the same capture handle — the same behavior as Linux, no extra setup.
- If you see *"no packets from Quest … can't see its traffic"*, this Mac isn't in the path — use
  `--sniff-mitm`, a mirror/SPAN port, or run on the gateway.

Point your app's microphone at **BlackHole** to hear the owner's voice.

---

## 5. What's not on macOS

- **Video / screen capture** to the Quest is not ported (no CoreGraphics/ScreenCaptureKit capture
  backend yet) — macOS is audio + control + the owner-mic sniffer.
- The room-mic and owner-mic can't be two *distinct* system mics with a single BlackHole device
  (see the caveat above).

## 6. Troubleshooting

| Symptom | Fix |
|---|---|
| `BlackHole not found` warning | `brew install blackhole-2ch`, then log out/in; check `system_profiler SPAudioDataType`. |
| App doesn't hear the mic | Select **BlackHole** as that app's microphone/input device. |
| You can't hear desktop audio | You set output to raw BlackHole — use a **Multi-Output Device** instead. |
| Sniffer captures nothing | This Mac can't see the Quest's traffic — use `--sniff-mitm` or a mirror port. |
| `pcap_activate: … permission` | Run via `sudo` (capture needs root). |
