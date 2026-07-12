# bsdrX — Android NDK build of the agent as a JNI shared library (libbsdr_agent.so).
#
# Reuses the portable C core from ../../../../../src verbatim (the same "compile
# the shared sources for another toolchain" trick the MinGW/osxcross builds use);
# only the platform shims differ. Deps come from android/deps/<abi> (built by
# scripts/build-android-deps.sh). No ffmpeg/X11/PulseAudio: capture is MediaCodec
# and audio is AudioRecord/AudioTrack, both in Kotlin.
cmake_minimum_required(VERSION 3.22)
project(bsdr_agent C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE "${REPO}/src")
set(DEPS "${REPO}/android/deps/${ANDROID_ABI}")

if(NOT EXISTS "${DEPS}/lib/libssl.a")
  message(FATAL_ERROR
    "Android deps for ${ANDROID_ABI} not found at ${DEPS}.\n"
    "Run: scripts/build-android-deps.sh ${ANDROID_ABI}")
endif()

# ---- portable core (identical to the desktop CORE_SRC) ---------------------
set(CORE_SRC
  ${CORE}/log.c ${CORE}/net.c ${CORE}/json.c ${CORE}/input_decode.c
  ${CORE}/discovery.c ${CORE}/control.c ${CORE}/udp_transport.c
  ${CORE}/dtls.c ${CORE}/cloud.c ${CORE}/cloud_stream.c
  ${CORE}/app.c ${CORE}/webui.c ${CORE}/overlay.c ${CORE}/httpc.c ${CORE}/tls.c
  ${CORE}/stt.c ${CORE}/llm.c ${CORE}/compcontrol.c ${CORE}/voice.c
  ${CORE}/screenshot.c                  # stubbed on Android (no X11/ffmpeg)
  ${CORE}/agent.c                       # bsdr_agent_run(); main() is #ifdef'd out
  # transport / media (shared, OS-agnostic)
  ${CORE}/sctp.c ${CORE}/srtp_util.c ${CORE}/video.c
  ${CORE}/threed.c                      # 2D->3D SBS core (agent.c refs it; not applied on the HW path)
  ${CORE}/depth_onnx.c ${CORE}/model_store.c   # in-process neural depth (ONNX Runtime, NNAPI)
  ${CORE}/webcam.c                      # camera enumeration (Android returns none; Kotlin enumerates)
  ${CORE}/voicefx.c                     # realtime voice changer (gender shift) DSP
  ${CORE}/faceswap.c                    # realtime face swap (SCRFD + ArcFace + inswapper, ONNX)
  ${CORE}/roomstate.c                   # room/avatar FlatBuffers state codec (bot Plane-2)
  ${CORE}/botroom.c                     # full-bot avatar presence (room data channel)
  ${CORE}/botaudio.c                    # cloud-mic loopback (bot room-audio -> BSDR_RoomMic)
  ${CORE}/voicestore.c                  # AI voice-changer model store + voice library
  ${CORE}/voiceai.c                     # RVC voice conversion (ONNX Runtime)
  ${REPO}/third_party/miniz/miniz.c     # model-zip import
  # flatcc runtime (vendored) backs roomstate.c's FlatBuffers codec
  ${REPO}/third_party/flatcc/runtime/builder.c
  ${REPO}/third_party/flatcc/runtime/emitter.c
  ${REPO}/third_party/flatcc/runtime/refmap.c
  ${CORE}/audio.c                       # Opus RTP/SRTP half (Pulse half #ifdef'd out)
  ${CORE}/micsniff.c                    # owner-mic RELAY path only (router companion) + voice FX
  ${CORE}/micsub.c                      # owner-mic cloud substitution (NFQUEUE) — stub on Android
  ${CORE}/deps.c                        # 3rd-party dependency manager (empty list on Android)
  ${CORE}/screenblank.c                 # privacy screen-blank (no-op on Android)
  ${CORE}/term.c                        # terminal source: Android compiles the stub (headless-only feature)
  # Android platform shims (replace capture.c / inject_*.c / pulse half / winlist / filesrc)
  ${CORE}/capture_android.c ${CORE}/audio_android.c ${CORE}/inject_android.c
  ${CORE}/winlist_android.c ${CORE}/filesrc_android.c ${CORE}/voice_android.c
  # JNI bridge
  ${CMAKE_CURRENT_SOURCE_DIR}/bsdr_jni.c)

add_library(bsdr_agent SHARED ${CORE_SRC})

# Vendored miniz: _LARGEFILE64_SOURCE selects miniz's fopen64/ftello64 branch on glibc (it defines
# __USE_LARGEFILE64), but Bionic doesn't honour that macro, so on Android miniz falls to its plain
# fopen/ftello/fseeko branch and emits a "may not support large files" #pragma message. That branch is
# actually correct here — the NDK compiles with _FILE_OFFSET_BITS=64 at API 29, so ftello/fseeko are
# already 64-bit — making the message a false positive. Silence just that pragma for the miniz TU.
set_source_files_properties(${REPO}/third_party/miniz/miniz.c PROPERTIES
  COMPILE_DEFINITIONS "_LARGEFILE64_SOURCE=1"
  COMPILE_OPTIONS "-Wno-#pragma-messages")

# Vendored flatcc runtime: compile warning-free like the Makefile does (-w).
set_source_files_properties(
  ${REPO}/third_party/flatcc/runtime/builder.c
  ${REPO}/third_party/flatcc/runtime/emitter.c
  ${REPO}/third_party/flatcc/runtime/refmap.c
  PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(bsdr_agent PRIVATE
  ${REPO}/include ${CORE} ${DEPS}/include ${REPO}/third_party/miniz
  ${REPO}/third_party/flatcc/include ${REPO}/src/generated)

target_compile_definitions(bsdr_agent PRIVATE
  BSDR_PLATFORM_ANDROID=1
  BSDR_NO_CLI_MAIN=1            # agent.c main() excluded; JNI calls bsdr_agent_run()
  BSDR_ENABLE_SCTP=1
  BSDR_ENABLE_VIDEO=1 BSDR_HAVE_CAPTURE=1
  BSDR_ENABLE_AUDIO=1 BSDR_HAVE_AUDIO=1
  BSDR_HAVE_ONNX=1)            # in-process depth via ONNX Runtime (NNAPI EP)

# prebuilt static deps
add_library(ssl     STATIC IMPORTED)
add_library(crypto  STATIC IMPORTED)
add_library(opus    STATIC IMPORTED)
add_library(srtp2   STATIC IMPORTED)
add_library(usrsctp STATIC IMPORTED)
set_target_properties(ssl     PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libssl.a)
set_target_properties(crypto  PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libcrypto.a)
set_target_properties(opus    PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libopus.a)
set_target_properties(srtp2   PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libsrtp2.a)
set_target_properties(usrsctp PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libusrsctp.a)

find_library(log_lib log)

# ONNX Runtime for the in-process depth tiers (NNAPI EP). The onnxruntime-android AAR ships no
# prefab metadata, so build-android-deps.sh extracts its headers + libonnxruntime.so into
# ${DEPS} (and the .so into src/main/jniLibs so it lands in the APK). Link it as a shared import.
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libonnxruntime.so)

# srtp2/ssl ordering matters for the static link; group them.
target_link_libraries(bsdr_agent
  opus srtp2 usrsctp ssl crypto onnxruntime ${log_lib})
