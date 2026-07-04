# bsdrX on Android

An Android port of the bsdrX agent: it casts the **Android device's screen** into a
Bigscreen VR headset over the LAN — the phone/tablet is the "desktop host". It reuses
the portable C core (`src/`) verbatim through the NDK; only the platform shims differ.

```
android/                      Gradle project (Kotlin app + NDK build of the core)
  app/src/main/java/…         MediaProjection, MediaCodec, AudioRecord/Track,
                              AccessibilityService, the control WebView, a bubble overlay
  app/src/main/cpp/           bsdr_jni.c (Kotlin <-> agent bridge) + CMakeLists.txt
src/*_android.c               the platform shims (capture/audio/inject/winlist/filesrc)
```

## Build

```bash
scripts/build-android-deps.sh arm64-v8a      # cross-build openssl/opus/srtp2/usrsctp
cd android && ./gradlew assembleDebug         # -> app/build/outputs/apk/debug/app-debug.apk
```

Needs the Android SDK (`local.properties: sdk.dir=…`), NDK, and `minSdk 29`
(AudioPlaybackCapture for system audio; the prebuilt deps also use `getrandom`,
API 28+). Only `arm64-v8a` is built by default (the common phone/Quest ABI).

## How the pieces map to the desktop agent

| Capability            | Desktop (Linux)          | Android                                            |
|-----------------------|--------------------------|---------------------------------------------------|
| Discovery / pair / DTLS / SCTP / SRTP | shared core | **shared core** (identical wire protocol)          |
| Video out             | X11 grab + NVENC         | **MediaProjection + MediaCodec** (AVC), fed to the core video sender via `bsdr_android_push_video` |
| Audio out (device→headset) | PulseAudio monitor  | **AudioPlaybackCapture** → `bsdr_android_push_audio` |
| Audio in (headset mic→device) | virtual mic       | **AudioTrack** via `bsdr_android_emit_mic`         |
| Input inject          | uinput                   | **AccessibilityService** (gestures + keys) via `bsdr_android_emit_input` |
| Control panel         | browser at :8088         | **in-app WebView** to the same native web UI       |
| Internet sharing (cloud relay) | shared cloud_stream | **shared** (compiled in)                           |
| Voice computer control | owner-mic sniffer | **device microphone** → same STT/LLM/tools → AccessibilityService |
| Stream a local file | ffmpeg demux passthrough | **MediaExtractor → transcode → same video path** (any input codec) |
| Vision screenshot (voice tool) | X11 grab → JPEG | **MediaProjection + ImageReader → JPEG** |

## Not applicable / deferred on Android

- **Owner-mic sniffer** — needs promiscuous capture (root); not applicable. The
  sniffer sources are simply left out of the NDK build (`micsniff.h` provides inert
  stubs when `BSDR_PLATFORM_ANDROID` is defined).
- **Voice computer control** — **wired** via the device's own microphone (Android has
  no sniffer). Enable it in the control panel (Computer control card; only an LLM
  endpoint is required — no owner-mic gate on Android). The agent then starts a
  `VoiceMic` (48 kHz mono, VOICE_RECOGNITION source) that feeds the same voice VAD →
  STT → LLM → tool loop as desktop; tools execute through the AccessibilityService
  injector. The floating **bubble** is the trigger: tap to start listening, tap to
  stop, tap again to Send (a long-press opens the control panel). STT/LLM endpoints
  are configured exactly as on desktop. C bridge: `voice_android.c` + `bsdr_jni.c`;
  UI: `VoiceMic.kt` + `BubbleOverlay`/`BsdrService`.
- **In-VR overlay control bar** — deferred; the WebView + the floating bubble are the
  control surface on Android.

## Implemented differently from the desktop

- **Stream a local file** — "Stream a video file" in the app opens a picker; a
  `FileSource` (MediaExtractor → MediaCodec decoder → the AVC encoder's input surface →
  the same `nativePushVideo` path) transcodes any input to Quest-compatible H.264 and
  loops it, paced to the file's timestamps. No MediaProjection needed (a `dataSync`
  foreground service). The C `filesrc_android.c` stays a stub — the source swap is
  Kotlin-side, so the agent just casts whatever access units it's fed.
- **Vision screenshot** — the voice model's `take_screenshot` tool works: `screenshot.c`
  routes through the JNI bridge to `Screenshotter` (a one-shot VirtualDisplay + ImageReader
  off the live MediaProjection → JPEG). Only in screen-cast mode (file mode has no screen).

Everything else is the same portable core as the desktop builds, so protocol,
pairing, quality control, and internet sharing behave identically.
