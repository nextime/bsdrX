cmake_minimum_required(VERSION 3.16)
project(bsdrX C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- options ---------------------------------------------------------------
option(BSDR_ENABLE_SCTP "Build the usrsctp DataChannel transport" OFF)
option(BSDR_ENABLE_VIDEO "Build the video sender (SRTP + capture/encode)" OFF)
option(BSDR_ENABLE_AUDIO "Build the audio sender/receiver (Opus + PulseAudio)" OFF)
option(BSDR_BUILD_TESTS "Build unit tests" ON)
option(BSDR_BUILD_TOOLS "Build the dtls_probe / test_client tools" ON)

if(MSVC)
  add_compile_options(/W3)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---- dependencies ----------------------------------------------------------
find_package(OpenSSL REQUIRED)   # DTLS-SRTP transport (Linux/Win/macOS)

# ---- platform-specific input-injection backend -----------------------------
if(WIN32)
  set(BSDR_INJECT_SRC src/inject_win.c)
elseif(APPLE)
  set(BSDR_INJECT_SRC src/inject_macos.c)
elseif(UNIX)
  set(BSDR_INJECT_SRC src/inject_linux.c)
else()
  set(BSDR_INJECT_SRC src/inject_null.c)
endif()

# ---- core library ----------------------------------------------------------
set(BSDR_CORE_SRC
  src/log.c
  src/net.c
  src/json.c
  src/input_decode.c
  src/discovery.c
  src/control.c
  src/udp_transport.c
  src/dtls.c
  src/transport.c
  src/cloud.c
  src/app.c
  src/webui.c
  src/overlay.c
  src/httpc.c
  src/stt.c
  src/llm.c
  src/compcontrol.c
  src/voice.c
  src/screenshot.c
  ${BSDR_INJECT_SRC})

if(BSDR_ENABLE_SCTP)
  list(APPEND BSDR_CORE_SRC src/sctp.c)
endif()

if(BSDR_ENABLE_VIDEO OR BSDR_ENABLE_AUDIO)
  list(APPEND BSDR_CORE_SRC src/srtp_util.c)   # shared SRTP helpers
endif()
if(BSDR_ENABLE_VIDEO)
  list(APPEND BSDR_CORE_SRC src/video.c)
  if(UNIX AND NOT APPLE)
    list(APPEND BSDR_CORE_SRC src/capture.c src/filesrc.c)  # NVENC + file demux
  endif()
endif()
if(BSDR_ENABLE_AUDIO AND UNIX AND NOT APPLE)
  list(APPEND BSDR_CORE_SRC src/audio.c src/micsniff.c src/micsniff_capture.c)   # Opus + PulseAudio; owner-mic sniffer
  find_library(PCAP_LIB pcap)   # portable capture engine; AF_PACKET fallback if absent
  if(PCAP_LIB)
    add_compile_definitions(BSDR_HAVE_PCAP=1)
  endif()
endif()

add_library(bsdr_core STATIC ${BSDR_CORE_SRC})
target_include_directories(bsdr_core PUBLIC include)
target_link_libraries(bsdr_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# threads / sockets / platform libs
if(WIN32)
  # ws2_32: sockets; user32: SendInput; bcrypt: CSPRNG; crypt32: static OpenSSL dep
  target_link_libraries(bsdr_core PUBLIC ws2_32 user32 bcrypt crypt32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(bsdr_core PUBLIC Threads::Threads)
endif()
if(APPLE)
  target_link_libraries(bsdr_core PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
endif()

if(BSDR_ENABLE_SCTP)
  target_compile_definitions(bsdr_core PUBLIC BSDR_ENABLE_SCTP=1)
  find_library(USRSCTP_LIB usrsctp REQUIRED)
  target_link_libraries(bsdr_core PUBLIC ${USRSCTP_LIB})
endif()

if(BSDR_ENABLE_VIDEO OR BSDR_ENABLE_AUDIO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SRTP2 REQUIRED libsrtp2)
  target_include_directories(bsdr_core PUBLIC ${SRTP2_INCLUDE_DIRS})
  target_link_libraries(bsdr_core PUBLIC ${SRTP2_LIBRARIES})
endif()
if(BSDR_ENABLE_VIDEO)
  target_compile_definitions(bsdr_core PUBLIC BSDR_ENABLE_VIDEO=1)
  if(UNIX AND NOT APPLE)
    pkg_check_modules(FFMPEG REQUIRED
      libavcodec libavformat libavdevice libavutil libswscale)
    target_compile_definitions(bsdr_core PUBLIC BSDR_HAVE_CAPTURE=1)
    target_include_directories(bsdr_core PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(bsdr_core PUBLIC ${FFMPEG_LIBRARIES})
  endif()
endif()
if(BSDR_ENABLE_AUDIO AND UNIX AND NOT APPLE)
  pkg_check_modules(OPUS REQUIRED opus)
  pkg_check_modules(PULSE REQUIRED libpulse-simple)
  target_compile_definitions(bsdr_core PUBLIC BSDR_ENABLE_AUDIO=1 BSDR_HAVE_AUDIO=1)
  target_include_directories(bsdr_core PUBLIC ${OPUS_INCLUDE_DIRS} ${PULSE_INCLUDE_DIRS})
  target_link_libraries(bsdr_core PUBLIC ${OPUS_LIBRARIES} ${PULSE_LIBRARIES})
  if(PCAP_LIB)
    target_link_libraries(bsdr_core PUBLIC ${PCAP_LIB})   # owner-mic sniffer capture backend
  endif()
endif()

# ---- main agent ------------------------------------------------------------
add_executable(bsdr_agent src/agent.c)
target_link_libraries(bsdr_agent PRIVATE bsdr_core)

# ---- tools -----------------------------------------------------------------
if(BSDR_BUILD_TOOLS)
  add_executable(dtls_probe tools/dtls_probe.c)
  target_link_libraries(dtls_probe PRIVATE bsdr_core)
  add_executable(test_client tools/test_client.c)
  target_link_libraries(test_client PRIVATE bsdr_core)

  add_executable(voice_test tools/voice_test.c)
  target_link_libraries(voice_test PRIVATE bsdr_core m)

  if(BSDR_ENABLE_VIDEO AND UNIX AND NOT APPLE)
    add_executable(video_capture_test tools/video_capture_test.c)
    target_link_libraries(video_capture_test PRIVATE bsdr_core)
    add_executable(stream_demo tools/stream_demo.c)
    target_link_libraries(stream_demo PRIVATE bsdr_core)
  endif()
  if(BSDR_ENABLE_AUDIO AND UNIX AND NOT APPLE)
    add_executable(audio_test tools/audio_test.c)
    target_link_libraries(audio_test PRIVATE bsdr_core)
  endif()
endif()

# ---- tests -----------------------------------------------------------------
if(BSDR_BUILD_TESTS)
  enable_testing()
  add_executable(test_input_decode tests/test_input_decode.c)
  target_link_libraries(test_input_decode PRIVATE bsdr_core)
  add_test(NAME input_decode COMMAND test_input_decode)

  add_executable(test_protocol tests/test_protocol.c)
  target_link_libraries(test_protocol PRIVATE bsdr_core)
  add_test(NAME protocol COMMAND test_protocol)

  add_executable(test_overlay tests/test_overlay.c)
  target_link_libraries(test_overlay PRIVATE bsdr_core)
  add_test(NAME overlay COMMAND test_overlay)

  add_executable(test_compcontrol tests/test_compcontrol.c)
  target_link_libraries(test_compcontrol PRIVATE bsdr_core)
  add_test(NAME compcontrol COMMAND test_compcontrol)

  add_executable(test_voice tests/test_voice.c)
  target_link_libraries(test_voice PRIVATE bsdr_core)
  add_test(NAME voice COMMAND test_voice)

  add_executable(test_transport tests/test_transport.c)
  target_link_libraries(test_transport PRIVATE bsdr_core)
  add_test(NAME transport COMMAND test_transport)

  if(BSDR_ENABLE_SCTP)
    add_executable(test_datachannel tests/test_datachannel.c)
    target_link_libraries(test_datachannel PRIVATE bsdr_core)
    add_test(NAME datachannel COMMAND test_datachannel)
  endif()

  if(BSDR_ENABLE_VIDEO)
    add_executable(test_video tests/test_video.c)
    target_link_libraries(test_video PRIVATE bsdr_core)
    add_test(NAME video COMMAND test_video)

    add_executable(test_stream tests/test_stream.c)
    target_link_libraries(test_stream PRIVATE bsdr_core)
    add_test(NAME stream COMMAND test_stream)
  endif()

  if(BSDR_ENABLE_AUDIO AND UNIX AND NOT APPLE)
    add_executable(test_audio tests/test_audio.c)
    target_link_libraries(test_audio PRIVATE bsdr_core m)
    add_test(NAME audio COMMAND test_audio)
  endif()
endif()
