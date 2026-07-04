# bsdrX — Plan: multi-headset streaming + on-the-fly 2D→3D (SBS)

Status: design/plan only (no code changed). Targets the C agent in `bsdrX/`.
Assumes the current working pipeline: per-headset **DTLS server on 45004** (→ SCTP DataChannel
for input), **45002** media channel (magic beacon `0x01234567` + SRTP H.264 video, PT 111,
keys from the 45004 DTLS exporter, AES_CM_128_SHA1_80), resolution/bitrate driven live via
`PUT /device` and `bsdr_transport_set_quality`.

---

## Feature 1 — Multiple concurrent headsets (Solution A)

**Decisions (from the brief):**
- **Solution A:** one UDP socket per fixed port (45002, 45004), demultiplex inbound datagrams
  by **source IP** into the right per-headset session.
- **One controller headset:** only the controller's input is injected; all others are view-only.
- **One shared encoder:** all headsets see the same video. **Resolution/bitrate are set from the
  web UI** (on the controller) and apply to the shared encoder **live**.

### Current blockers (single-headset assumptions)
- `control.c`: a single `have_device`/`device`; `/pair` returns "Already paired" for a 2nd headset.
- `agent.c`: one `agent_t`, one `transport`, one `worker`, one `remote_ip`.
- `transport.c`: `bsdr_udp udp` (45004) + `media_udp` (45002) each pinned to **one** Quest IP via
  `bsdr_udp_open(..., remote_ip, ...)`; the pump `bsdr_transport_run` reads one socket for one peer.
- `dtls.c`/`sctp.c`/`video.c` send through a single `bsdr_udp` whose remote is one peer.
- `app.c`: single `quest_*`, `selected_quest_ip`, `blocked_quest_ip`.

### Target architecture
**Shared (one global instance):**
- Desktop capture + H.264 encode (the expensive part — encode once).
- The `bsdr_injector` (one real desktop) — fed only by the controller session.
- Two shared UDP sockets: `0.0.0.0:45004` and `0.0.0.0:45002`.
- Web UI / `bsdr_app` state, control HTTP server, discovery.

**Per-session (`bsdr_session`, one per connected headset, cap e.g. `BSDR_MAX_SESSIONS=8`):**
- `remote_ip` + cached `sockaddr_in` for 45002 and 45004.
- DTLS server state (own handshake + own SRTP exporter keys).
- SCTP association (own DataChannel) — input.
- SRTP **video send context** (own keys + own SSRC).
- Media-beacon timer, heartbeat/last-keepalive, pairing_id, device_name.
- `bool is_controller`.

### Key refactor: socket → endpoint + central demux
Today `bsdr_dtls`/`bsdr_sctp`/`bsdr_video_sender` hold a `bsdr_udp*` and send to its single pinned
remote. Introduce a lightweight **endpoint**: `{ shared_fd, struct sockaddr_in remote }` and route
all sends through `sendto(shared_fd, remote)`. Inbound becomes centralized:

- A single **demux pump** per shared socket: `recvfrom` → look up session by **source IP** →
  feed that session's DTLS (45004) or media handler (45002). New source on 45004 with a DTLS
  ClientHello + an allowed/paired IP ⇒ spin up a session.
- `bsdr_dtls_new` / `bsdr_sctp_new` / `bsdr_video_sender_new` take an **endpoint** instead of a
  connected `bsdr_udp`. (Backwards-compatible: single-session path is just N=1.)

usrsctp note: multiple SCTP associations over one UDP/DTLS demux is supported by usrsctp, but our
single-threaded `usrsctp_handle_timers` driving must iterate all sessions; keep the existing
`usrsctp_init_nothreads` model and tick every session each pump iteration.

### Encode-once, fan-out
- One capture/encode thread produces an H.264 access unit (Annex-B) per frame.
- For each active session: packetize (RFC 6184 FU-A/STAP — can be shared) then **SRTP-protect with
  that session's keys + SSRC** and `sendto` its 45002. SRTP protect is necessarily per-session;
  encode is shared. (`video.c` split: `packetize()` once → per-session `srtp_protect()+send`.)
- Keep RTP sequence numbers and timestamps per session.

### Controller, input, resolution
- `bsdr_app` gains `controller_ip` (set from the UI; defaults to the first/most-recent headset).
- Only the controller session's SCTP input messages reach the global injector; non-controller
  input is dropped (don't inject). Overlay/voice actions likewise gated to the controller.
- Resolution/bitrate: UI → `bsdr_app_set_quality` → `settings_dirty` → live reconfig of the
  **shared** encoder (already implemented). All headsets share one resolution. (Per-headset
  resolution would require per-headset encoders — out of scope by decision.)
- The controller's own `PUT /device` may also drive quality; the UI is authoritative.

### Pairing / discovery / control
- `control.c`: replace the single device with `bsdr_paired_device devices[BSDR_MAX_SESSIONS]`.
  `/pair` allowed while count < cap and `allow_pair(ip)` passes; each gets its own `pairingId`.
  `/heartbeat`, `/start`, `/stop`, `/unpair`, stale-expiry become per-pairingId.
- `--quest_ip` becomes **repeatable** (allow-list); empty list = accept any. UI also manages the
  allow-list and the controller selection.

### Web UI
- List connected headsets (name/IP, streaming state). Radio to pick the **controller**.
  Resolution/bitrate controls apply to the shared encoder. Per-headset Disconnect.

### Phases
- **M1** Endpoint abstraction + central demux-by-source on 45004/45002; refactor dtls/sctp/video
  to endpoints. Keep N=1 behaving exactly as today. (Largest, riskiest step.)
- **M2** Multi-pair in `control.c` (array, per-pairingId lifecycle).
- **M3** `bsdr_session` list; per-session DTLS/SCTP/SRTP; encode-once + SRTP fan-out.
- **M4** Controller designation + input gating; UI list + controller radio + per-headset disconnect.
- **M5** Live UI-driven resolution/bitrate to the shared encoder (mostly exists; wire to UI).

### Risks / notes
- Demux refactor touches the hottest path — do M1 behind the existing single-session behavior.
- usrsctp multi-association timer driving and teardown ordering need care (leaks/UAF on
  disconnect). Reuse the crash-safe teardown discipline already in the injector.
- SRTP fan-out CPU scales with N; encode stays O(1). Fine for a handful of viewers.
- Heartbeat-expiry + operator "disconnect" + the blocked-IP logic all become per-session.

---

## Feature 2 — On-the-fly 2D → 3D (Side-by-Side)

**Decision (from the brief):** the headset already has a **3D format toggle (SBS/OU)**, so the
display side is solved. We only need to **produce a stereo (SBS) frame** from the 2D desktop and
feed it to the existing H.264 encoder; the user sets the headset's 3D toggle to match.

### Where it slots in
Insert a **stereoizer** stage in the capture path, before NV12/encode. Today (`capture.c`):
`x11grab decode → sws_scale → NV12 → NVENC`. New: `x11grab decode → stereoize → SBS frame →
sws/NV12 → NVENC`. A new module `src/video_stereo.c` (+ `include/bsdr/video_stereo.h`).

### Output format
- **Half-SBS** (recommended default): left eye in the left half, right eye in the right half, each
  horizontally squeezed to half width; total frame keeps the target resolution (e.g. 1280×720).
  Matches the most common headset "SBS" toggle, no extra bandwidth.
- **Full-SBS** optional: 2×width (e.g. 2560×720) — sharper per eye, ~2× pixels to encode/send.
- Whatever we choose, it's just a normal H.264 frame to the transport; the headset's toggle splits
  it. Resolution control (480/720/1080) applies to the SBS frame as a whole.

### Conversion approaches (cheap → best)
1. **D-A: Geometric "pop" (no depth).** Build L/R by shifting the whole frame by a fixed disparity
   (or a depth ramp by screen-Y). Near-zero cost; gives a flat window-depth, not true 3D. Good for
   validating the SBS plumbing + the headset toggle end-to-end.
2. **D-B: Monocular depth + DIBR (true depth).** Estimate a depth map with a small monocular model
   (Depth-Anything-Small / MiDaS-small) via ONNX Runtime or TensorRT (CUDA), then depth-image-based
   rendering: warp the frame to L/R using per-pixel disparity ∝ depth, fill occlusion holes
   (horizontal inpaint/stretch). Real depth; GPU-heavy.
3. **D-C: 2D+Depth (if the headset's 3D mode accepts it).** Send frame + half-width depth map and
   let the headset do the DIBR. Offloads the warp/inpaint to the headset — we only run depth
   estimation. Cheapest high-quality path **if** the toggle supports a 2D+Depth format; needs a
   quick check of the headset's available 3D modes.

### GTX 1650 budget
- The depth NN is the bottleneck. Mitigations: run depth at **reduced resolution** and **reduced
  rate** (e.g. 10–15 Hz) and reuse/interpolate the disparity field between frames; use a small
  model + TensorRT FP16; cap SBS at 720p. Realistic first target: **720p half-SBS, depth ~10–15 Hz**.
- NVENC encode of the SBS frame is the same cost as a normal frame of that resolution.
- D-A costs ~nothing; D-B competes with NVENC for the GPU — measure before committing to 1080p.

### Config / UX
- Web UI toggle: `3D: off | SBS-flat (D-A) | SBS-depth (D-B) | 2D+Depth (D-C)`, plus a disparity/
  "3D strength" slider. The user sets the headset's own 3D toggle to the matching format.
- **Interaction with multi-headset:** the encoder is **shared**, so 3D is a **global** (all-or-none)
  setting across headsets in the Solution-A design. Per-headset 3D would need per-headset encoders
  (explicitly out of scope). Document this in the UI (3D applies to everyone).

### Phases
- **D1** SBS plumbing: half-SBS framing via D-A (duplicate L/R), feed encoder; confirm the headset's
  SBS toggle renders it. Cheapest end-to-end validation of format + geometry.
- **D2** D-A polish: depth-ramp / fixed-disparity "3D strength" slider; full-SBS option.
- **D3** D-B: integrate small monocular depth (ONNX/TensorRT) + DIBR warp + occlusion fill.
- **D4** Perf: depth at reduced res/rate, disparity reuse, resolution caps; optional D-C 2D+Depth
  path if the headset supports it (verify modes first).
- **D5** Web UI controls (mode + strength); document the headset-toggle pairing and the global
  (shared-encoder) nature.

### Risks / notes
- Verify the headset's exact accepted 3D input format (half-SBS vs full-SBS vs OU vs 2D+Depth)
  early — D1 settles it cheaply before any NN work.
- DIBR occlusion artifacts are the main quality risk; budget time for hole-filling.
- Depth NN + NVENC contention on the 1650 may force 720p / reduced depth rate.

---

## Suggested order
1. Multi-headset **M1** (endpoint + demux) — unblocks everything and is the riskiest plumbing.
2. 3D **D1** (SBS-flat) in parallel — independent of multi-headset, cheap, de-risks the format.
3. Then M2–M5, then D2–D5. 3D is a global encoder setting under Solution A — finalize that UX once
   multi-headset M3 (shared encode + fan-out) lands.
