# bsdrX — STATUS / resume-here

> Single source of truth for "where is this project". For the LAN/media protocol see the memory
> notes and `docs/`; for the plugin ABI see [docs/PLUGIN-ABI.md](docs/PLUGIN-ABI.md); for signing
> [docs/WINDOWS-SIGNING.md](docs/WINDOWS-SIGNING.md) / [docs/MACOS-SIGNING.md](docs/MACOS-SIGNING.md).
> Git: **origin = git.nexlab.net:nextime/bsrdx.git** (github is a mirror — push origin only).
> Last updated: 2026-07-18.

## What bsdrX is
A native, cross-platform (**Linux / Windows / macOS / Android**) agent in **C** that hosts the PC
side of the Bigscreen VR **Remote Desktop** LAN protocol: UDP discovery (45000), HTTP control/pairing
(45678), media/DTLS transport (45002/45004), and a control web UI on `127.0.0.1:8088`. It streams the
desktop (or a file / webcam / terminal / **virtual desktop**) to a paired headset with the Quest's
keyboard+mouse injected, plus effects the official client lacks (real-time 2D→3D, face swap, voice
changer/RVC), an in-room moderator bot (plugin-based), internet/cloud sharing, and an owner-mic path.
Builds two binaries: **`bsdr_agent`** and the router companion **`bsdr_micrelay`**.

## Build / run quick reference
```
make -j"$(nproc)"                      # -> build/bsdr_agent (+ tools + tests)
make micrelay                          # -> build/bsdr_micrelay (router companion)
make check                             # loopback unit suites
.claude/skills/run-bsdrx/smoke.sh      # headless build+launch+web-UI+shutdown smoke -> ALL GREEN
scripts/build-micrelay.sh              # cross-build the static relay bundle -> dist/bsdrX_relay.zip
```

---

## 2026-07-18 — owner-mic RELAY OVERHAUL: auto-discovery + multi-headset + bind-to-owner
Committed **b32c8e2** (pushed to origin). Turned `bsdr_micrelay` from a hand-wired single-headset
tool into a zero-config, multi-flow control plane. Shared wire contract in the new
**`include/bsdr/relayproto.h`** (magic `BSRL` + version + type; UDP **45099**).

- **Mutual auto-discovery** — the relay (`bsdr_micrelay --iface br-lan`, nothing else) broadcasts a
  HELLO beacon; each `bsdr_agent` hears it, learns the relay address, and unicasts REGISTER (~1s
  heartbeat) for the headset it is paired with. The REGISTER source IS the mic-return path, so no
  forward-port is configured. Bootstrap = broadcast REGISTER until the beacon arrives.
- **bsdrX drives the config** — the agent already knows its paired headset IP (`a->remote_ip`) and
  the cloud audio dst (learned flow), and hands both to the relay; the relay installs the flow.
- **Bind-to-owner auth** (operator-chosen) — the relay is in-path, so it observes each headset↔agent
  bsdrX session on the pairing ports (`BSDR_RELAY_PAIR_PORTS` = 45000/45002/45004) and only forwards
  a headset's mic to the agent it actually saw paired with it (ACK ok vs `DENY_UNPAIRED`, 20s TTL).
  A rogue LAN host cannot siphon someone else's owner voice.
- **Parallel (one relay, many agents)** — `mc_cap_open(iface, NULL, …)` now captures ALL UDP (new
  all-UDP BPF / AF_PACKET filter in `micsniff_capture.c`); the relay demuxes by source IP against a
  32-entry flow table and forwards each registered headset's uplink to its own agent. Flows expire 4s
  after the last heartbeat; UNREGISTER on agent stop.
- **Per-flow voice substitution** — `'C'`/`'F'`/`'A'` messages now carry `quest_ip` so the relay keys
  the iptables DROP + raw-inject to the right flow. The local macOS BPF-helper path is UNCHANGED
  (only the relay-directed messages got the version byte + quest tag). Legacy `--quest/--to` still
  works as a static single flow (skips discovery + pairing check).
- **Port coupling** — the beacon and each agent's listen socket share one port: default **45099** on
  both = truly zero-config; a custom relay port needs `bsdr_micrelay --port N` to match the agent's
  relay port (the web-UI `relayPort`, e.g. a persisted 6767).

**Files**: `include/bsdr/relayproto.h` (new), `tools/bsdr_micrelay.c` (rewrite), `src/micsniff.c`
(relay_register/discovery/UNREGISTER + per-flow tags), `src/micsniff_capture.c` (all-UDP capture),
`src/agent.c` (default port + help), `docs/bsdr_micrelay.1`, `README.md`, `scripts/build-micrelay.sh`.

**Validation** — built clean (agent + relay); on real capture (lo + a `1.2.3.4/32` lo alias as a
"public" mic dst): pairing-observed REGISTER → **ACK OK**, unpaired → **DENY**, and **5/5 mic packets
demuxed + forwarded** to the right agent. Relay bundle rebuilt for all 6 arches
(`dist/bsdrX_relay.zip`: amd64/x86/arm64/armv7/mipsel/mips).

**Deploy note** — the wire format changed, so **rebuild + redeploy the router binary AND the agents
together**; an old relay ignores the new REGISTER/tagged-substitution messages.

**Open item** — needs a **live router + Quest test** to confirm bind-to-owner pairing observation and
per-flow NAT reuse (masquerade + raw inject) hold on real hardware. See the memory note
`bsdrx-owner-mic-sniffer` for the full owner-mic history this builds on.

---

## 2026-07-18 — virtual desktop on headless Linux (same commit b32c8e2)
`--virtual-desktop[=SESSION]` — the xvfb terminal path (private Xvfb + x11grab + XTEST) now launches a
**full desktop** instead of a bare xterm, so a box with no monitor/Xorg is driven from the headset
like a real PC. No arg → the machine's configured default X session (`x-session-manager` →
GNOME/KDE/XFCE/…), falling back to a lightweight WM (openbox/…) + xterm if the DE can't come up
headless; `=SESSION` runs a custom session command. Plumbed through `bsdr_term_config.desktop`, the
app config (`term_desktop`, persisted), CLI, `--help`, README. (Also widened the smoke shutdown wait
2s→6s — full-plugin teardown takes ~3s; the clean-exit invariant is intact.)

---

## Recent workstreams (state carried in the auto-memory index)
Compact pointers; each has a detailed memory note.
- **Plugins & Plugin Store** — in-app store card + ABI **v7**; bot split into plugins; plugstore
  daemon live at `bigscreen.nexlab.net/bsdrxstore`; media-effect plugins (voice/faceswap/2d-3d);
  GPU provider shipped as the `gpu-cuda` payload plugin (public).
- **GPU across platforms** — ONNX EPs: CUDA (Linux, downloadable provider), DirectML (Windows),
  CoreML (macOS), NNAPI (Android); voice-changer RVC / 2D→3D / faceswap tiers.
- **Media/protocol** — LAN wire format cracked (video trailer, XOR-0x14); cloud/internet relay path
  (comedia/PlainTransport, jrtplib + DTLS-SRTP); owner-mic (this doc's headline).
- **Native app window** default on all desktops (browser optional); tray; code-signing wired
  (Azure Trusted Signing / Apple Developer ID, signed locally).
- **Cross-platform** — Windows media port, macOS parity, Android client is the separate **bsandroid**
  repo (consumes bsdrX media).

## Known open items / needs-live-test
- **Relay overhaul** — live router+Quest test (above).
- Owner-mic end-to-end only lights up with **≥2 users in the room** (Quest gates mic TX on
  `Room.Participants > 1`) — test with the second account joined + mic un-muted.
- Cloud video trailer crash fix, avatar data-channel prefix, and several plugin features carry
  "needs live-Quest test" flags (see the memory notes).
