# Voice computer control (the in-VR balloon)

bsdrX can let the headset owner **drive the Linux desktop by voice**. You speak; an
LLM decides which desktop actions to run (type text, key combos, click, scroll,
launch apps) and performs them through the same virtual input device that carries
the headset's mouse/keyboard.

The trigger is a small **balloon** drawn over the desktop you stream to the Quest.
Drag it anywhere with the controller pointer; **click it to talk**. It listens
until you stop speaking (silence detection) — or until you click it again — then
shows **Send / Cancel**. On Send it transcribes the audio and hands the text to
the model. While the model acts, a red **stop balloon** appears; click it to abort.
A feedback bubble under the balloon shows what's happening, and a small log handle
re-opens the recent history.

```
 Quest owner speaks ──▶ owner-mic sniffer (LAN) ──▶ STT (whisper) ──▶ LLM + tools ──▶ uinput
        ▲                                                                                │
        └──────────────── balloon click (input channel) ── arms one capture ────────────┘
```

## Why the owner-mic sniffer is required

The Bigscreen remote-desktop protocol has **no mic-upload channel** — the headset
never sends its microphone to this PC. The owner's voice only goes to the Bigscreen
*room* (the mediasoup cloud), as plain Opus RTP. bsdrX intercepts that stream off
the LAN (see [`../src/micsniff.c`](../src/micsniff.c) and the owner-mic section of
the README). That intercepted voice is the **only** source for computer control, so
**computer control can only be enabled while the owner mic (sniffer or MITM) is
running**. The web UI enforces this, and so does the agent.

## Configure

Everything is set in the local web panel (default <http://127.0.0.1:8088>), under
**Voice assistant**:

- **Speech-to-text (STT)** — leave the URL blank to use a built-in **free, keyless**
  online service (no setup). For private/faster results, point it at your own
  whisper-server, e.g. `http://localhost:8080/inference`, or any OpenAI-compatible
  `/v1/audio/transcriptions` endpoint, plus its model and token.
- **Language model (LLM)** — required. Any OpenAI-compatible chat endpoint
  (`https://api.openai.com/v1/chat/completions` or a local one), with model and
  bearer token. This is what decides and runs the desktop actions.

Then:

1. Start the **owner mic** (the "Headset owner mic" card — passive or MITM).
2. In **Computer control**, click **Enable computer control**. The button is only
   active once the owner mic is running and an LLM endpoint is set.

The balloon appears over the streamed desktop in VR. Disable at any time to hide it.

3. Optionally tick **Vision** so a vision-capable model can pull a screenshot when
   a request needs it (see below).

### CLI

```
bsdr_agent --sniff-mic  --compctl              # passive sniff + arm computer control
bsdr_agent --sniff-mitm --compctl              # ARP-MITM sniff + arm computer control
bsdr_agent --sniff-mic  --compctl-vision       # + offer the on-demand screenshot tool
bsdr_agent --sniff-mic  --compctl --listen-max 120 --confirm-timeout 30
```

`--compctl` arms it at startup; you still set the STT/LLM endpoints in the web panel
(or leave STT blank for the free service). It stays toggleable from the panel.

## Using it in VR

- **Move** the balloon: press on it and drag — it follows the pointer.
- **Talk**: click it (press + release without dragging). It glows **red** while
  listening. Speak; it stops automatically after a short silence, or click the
  balloon again to stop immediately.
- **Send / Cancel**: after listening stops, the balloon turns amber and a
  **Send** / **Cancel** row appears. Click Send to run it, Cancel to discard. If you
  do nothing it auto-cancels after the confirm timeout (default 1 min).
- **Stop a running command**: while the model is acting, a red **stop balloon**
  appears next to the main one — click it to abort the remaining actions.
- **Feedback + history**: a bubble under the balloon shows the current status
  (listening / transcript / thinking / result) for a few seconds. Click the small
  **log handle** just beneath the balloon to open the recent history; click again to
  close.

Examples: *"open firefox"*, *"select all and copy"*, *"scroll down"*,
*"type dear team newline"*, *"press alt f4"*, and (with Vision) *"click the blue
Save button"*.

## Vision (on demand)

With Vision enabled, the model is given an extra `take_screenshot` tool. It calls it
**only when the request needs it** to see the screen (find a control, read content,
decide where to click); the captured desktop is attached as an image for the model
to reason over. Requests that don't need the screen skip the screenshot. Use a
vision-capable model (e.g. `gpt-4o-mini`). The screenshot is JPEG, downscaled to
1280 px on the long side ([`../src/screenshot.c`](../src/screenshot.c)).

## Tuning

- **Listening ceiling** — `--listen-max SEC` (default **300 = 5 min**). Silence ends a
  capture much sooner; this is a safety cap. The captured audio buffer itself is
  bounded to ~120 s.
- **Confirm timeout** — `--confirm-timeout SEC` (default **60**). How long the
  Send/Cancel prompt waits before auto-cancelling.
- **VAD** — thresholds live in [`../src/voice.c`](../src/voice.c) (`apply_defaults`):
  `start_ms` (wait for speech, 4 s), `silence_ms` (trailing quiet that ends a capture,
  0.9 s). The gate adapts to the ambient noise floor measured in the first 250 ms.

## Scope / limitations

- **Linux LAN desktop path.** The balloon is composited onto the captured desktop
  frame and clicks are intercepted on the LAN input channel (45004). It is not wired
  into the pure internet-relay input path (which also can't supply the owner mic).
- **Capture path.** The balloon is drawn on the source frame *before* scale/encode,
  so it works on the CPU path and the CUDA/VAAPI GPU encoders. The zero-copy
  `--kmsgrab` DRM capture has no CPU surface, so the balloon is skipped there (logged
  once) — use x11grab (default) or CUDA/VAAPI-from-x11grab.
- **Safety.** `open_app` only accepts a bare command word (shell metacharacters are
  rejected). The model is instructed to do nothing on unclear/unsafe requests. The
  actions run as the user running the agent — treat the LLM endpoint as trusted.
