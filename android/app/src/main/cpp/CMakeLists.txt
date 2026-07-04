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
  ${CORE}/audio.c                       # Opus RTP/SRTP half (Pulse half #ifdef'd out)
  # Android platform shims (replace capture.c / inject_*.c / pulse half / winlist / filesrc)
  ${CORE}/capture_android.c ${CORE}/audio_android.c ${CORE}/inject_android.c
  ${CORE}/winlist_android.c ${CORE}/filesrc_android.c ${CORE}/voice_android.c
  # JNI bridge
  ${CMAKE_CURRENT_SOURCE_DIR}/bsdr_jni.c)

add_library(bsdr_agent SHARED ${CORE_SRC})

target_include_directories(bsdr_agent PRIVATE
  ${REPO}/include ${CORE} ${DEPS}/include)

target_compile_definitions(bsdr_agent PRIVATE
  BSDR_PLATFORM_ANDROID=1
  BSDR_NO_CLI_MAIN=1            # agent.c main() excluded; JNI calls bsdr_agent_run()
  BSDR_ENABLE_SCTP=1
  BSDR_ENABLE_VIDEO=1 BSDR_HAVE_CAPTURE=1
  BSDR_ENABLE_AUDIO=1 BSDR_HAVE_AUDIO=1)

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

# srtp2/ssl ordering matters for the static link; group them.
target_link_libraries(bsdr_agent
  opus srtp2 usrsctp ssl crypto ${log_lib})
