# bsdrX Makefile — works two ways:
#
#   ./configure && make           configured host build (honors config.mk)
#   make                          build for the local OS (no configure needed)
#   make linux                    build for Linux            -> build-linux/
#   make windows WIN_DEPS=DIR     cross-build for Windows (= windows-media) -> build-windows-media/
#   make osx                      native build ON macOS (full media via configure)
#   make osxcross OSX_DEPS=DIR    cross-build macOS from Linux (osxcross)   -> build-osx/
#
#   Every target is full-feature (desktop capture + audio + SCTP + SRTP); there
#   is no core-only build. A build whose media deps are missing errors out.
#
#   make check                    build + run the test suite
#   make install                  install the host build to $(prefix) (DESTDIR ok)
#   make clean / make distclean   remove build artifacts (+ config.mk)
#
# config.mk (written by ./configure) is optional; when absent, sensible host
# defaults are detected below. Command-line overrides always win, which is how
# the per-OS targets cross-compile without disturbing config.mk.

-include config.mk
# when config.mk exists, make objects depend on it so re-running ./configure
# (e.g. toggling --static or --disable-media) forces a rebuild, not a stale relink
CONFIG_MK := $(wildcard config.mk)

MAKEFLAGS += --no-print-directory
BASE_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -Ithird_party/miniz

# ---- host defaults (used only when config.mk did not set them) -------------
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  HOST_INJECT   := src/inject_macos.c
  HOST_PLATLIBS := -framework CoreGraphics -framework CoreFoundation
else
  HOST_INJECT   := src/inject_linux.c
  HOST_PLATLIBS := -lpthread -lm
endif
HOST_OSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
HOST_OSSL_LIBS   := $(shell pkg-config --libs openssl 2>/dev/null)
ifeq ($(strip $(HOST_OSSL_LIBS)),)
  HOST_OSSL_LIBS := -lssl -lcrypto
endif

# ---- in-process depth via ONNX Runtime (optional) --------------------------
# Point ONNX_PREFIX at an onnxruntime prebuilt (include/ + lib/); auto-detected from a sibling
# onnx-deps/ checkout or /opt/onnxruntime. Absent -> depth_onnx.c stubs out and the 2D->3D neural
# tiers report unavailable (the external helper + heuristic still work).
ONNX_PREFIX ?= $(firstword $(wildcard ../onnx-deps/onnxruntime-linux-* /opt/onnxruntime))
HOST_ONNX_DEF  :=
HOST_ONNX_LIBS :=
ifneq ($(strip $(ONNX_PREFIX)),)
  HOST_ONNX_DEF  := -DBSDR_HAVE_ONNX=1 -I$(ONNX_PREFIX)/include
  HOST_ONNX_LIBS := -L$(ONNX_PREFIX)/lib -lonnxruntime -Wl,-rpath,$(ONNX_PREFIX)/lib
endif

# ---- canonical media source groups (single source of truth) ----------------
# Every media-enabled build (host autodetect, windows-media, osxcross) derives
# its source list from these so the lists can't silently diverge. MEDIA_SRC_ALL
# is the full portable desktop set; the per-platform lists below add one platform
# audio backend (audio_wasapi.c / audio_coreaudio.c). `make print-media-src`
# emits $(sort $(MEDIA_SRC_ALL)) and scripts/build-linux-bundle.sh asserts its
# hardcoded MEDIA_SRC against it.
MEDIA_SRC_SCTP  := src/sctp.c
# NB: threed.c is in CORE_SRC, not here — agent.c's main() calls bsdr_threed_mode_parse
# unconditionally, so it must link in core-only builds too (the SBS transform it also
# provides is only *applied* on the BSDR_HAVE_CAPTURE path).
MEDIA_SRC_VIDEO := src/srtp_util.c src/video.c src/capture.c src/filesrc.c src/fileaudio.c src/capture_pipewire.c
MEDIA_SRC_AUDIO := src/audio.c src/micsniff.c src/micsniff_capture.c
MEDIA_SRC_ALL   := $(MEDIA_SRC_SCTP) $(MEDIA_SRC_VIDEO) $(MEDIA_SRC_AUDIO)

# media is implicit: auto-enable on Linux when the deps are present (config.mk
# from ./configure overrides this).
HOST_MEDIA_SRC :=
HOST_MEDIA_DEF :=
HOST_MEDIA_CFLAGS :=
HOST_MEDIA_LIBS :=
# winlist: webui.c always needs bsdr_list_windows; default to the null impl and
# upgrade to the X11 one only when the capture libs are present (Linux media).
HOST_WINLIST := src/winlist_null.c
ifeq ($(UNAME_S),Linux)
  ifneq ($(shell test -f /usr/include/usrsctp.h && echo 1),)
    HOST_MEDIA_SRC += $(MEDIA_SRC_SCTP)
    HOST_MEDIA_DEF += -DBSDR_ENABLE_SCTP=1
    HOST_MEDIA_LIBS += -lusrsctp
  endif
  ifneq ($(shell pkg-config --exists libsrtp2 libavcodec libavformat libavdevice libswscale libswresample libavfilter 2>/dev/null && echo 1),)
    HOST_MEDIA_SRC += $(MEDIA_SRC_VIDEO)
    HOST_MEDIA_DEF += -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1
    HOST_MEDIA_CFLAGS += $(shell pkg-config --cflags libsrtp2 libavcodec libavformat libavdevice libavutil libswscale libswresample libavfilter 2>/dev/null)
    HOST_MEDIA_LIBS += $(shell pkg-config --libs libsrtp2 libavcodec libavformat libavdevice libavutil libswscale libswresample libavfilter 2>/dev/null) -lX11
    HOST_WINLIST := src/winlist.c              # X11 window list (needs -lX11 above)
  endif
  ifneq ($(shell pkg-config --exists opus libpulse-simple 2>/dev/null && echo 1),)
    HOST_MEDIA_SRC += $(MEDIA_SRC_AUDIO)   # audio.c + owner-mic sniffer
    HOST_MEDIA_DEF += -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1
    # libpcap = portable capture engine; optional on Linux (AF_PACKET fallback otherwise)
    ifneq ($(shell pkg-config --exists libpcap 2>/dev/null && echo 1),)
      HOST_MEDIA_DEF += -DBSDR_HAVE_PCAP=1
      HOST_MEDIA_CFLAGS += $(shell pkg-config --cflags libpcap 2>/dev/null)
      HOST_MEDIA_LIBS += $(shell pkg-config --libs libpcap 2>/dev/null)
    endif
    HOST_MEDIA_CFLAGS += $(shell pkg-config --cflags opus libpulse-simple 2>/dev/null)
    HOST_MEDIA_LIBS += $(shell pkg-config --libs opus libpulse-simple 2>/dev/null)
    ifeq ($(filter src/srtp_util.c,$(HOST_MEDIA_SRC)),)
      HOST_MEDIA_SRC += src/srtp_util.c
      HOST_MEDIA_CFLAGS += $(shell pkg-config --cflags libsrtp2 2>/dev/null)
      HOST_MEDIA_LIBS += $(shell pkg-config --libs libsrtp2 2>/dev/null)
    endif
  endif
endif

# owner-mic SUBSTITUTION (NFQUEUE payload rewrite) — Linux host only, when libnetfilter_queue is
# present. Cross builds (windows/osx) override CFLAGS/LDLIBS and never see this -> micsub.c stub.
HOST_NFQUEUE_OK := $(shell pkg-config --exists libnetfilter_queue 2>/dev/null && echo 1)
ifeq ($(HOST_NFQUEUE_OK),1)
  HOST_NFQUEUE_DEF  := -DBSDR_HAVE_NFQUEUE=1
  HOST_NFQUEUE_LIBS := $(shell pkg-config --libs libnetfilter_queue)
endif

CC          ?= cc
AR          ?= ar
EXEEXT      ?=
BUILD       ?= build
INJECT_SRC  ?= $(HOST_INJECT)
WINLIST_SRC ?= $(HOST_WINLIST)
SCTP_SRC    ?=
MEDIA_SRC   ?= $(HOST_MEDIA_SRC)
CFLAGS      ?= $(BASE_CFLAGS) $(HOST_OSSL_CFLAGS) $(HOST_MEDIA_DEF) $(HOST_MEDIA_CFLAGS) $(HOST_ONNX_DEF) $(HOST_NFQUEUE_DEF)
LDFLAGS     ?=
LDLIBS      ?= $(HOST_OSSL_LIBS) $(HOST_PLATLIBS) $(HOST_MEDIA_LIBS) $(HOST_ONNX_LIBS) $(HOST_NFQUEUE_LIBS)
BUILD_TESTS ?= yes
prefix      ?= /usr/local
bindir      ?= $(prefix)/bin
libdir      ?= $(prefix)/lib
includedir  ?= $(prefix)/include
mandir      ?= $(prefix)/share/man
RUNNER      ?=

# cross-target knobs
WIN_HOST   ?= x86_64-w64-mingw32
WIN_DEPS   ?=                      # unified mingw prefix from scripts/build-win-deps.sh
# In-process depth on Windows: enabled when WIN_DEPS holds the onnxruntime headers. mingw links
# onnxruntime.dll directly (-l:); the DLL ships in the installer. DirectML EP is opt-in via a
# DirectML-enabled ORT (define BSDR_ONNX_DML + DirectML.dll); the default DLL is CPU/XNNPACK.
WIN_ONNX_DEF  = $(if $(wildcard $(WIN_DEPS)/include/onnxruntime_c_api.h),-DBSDR_HAVE_ONNX=1,)
WIN_ONNX_LIBS = $(if $(wildcard $(WIN_DEPS)/include/onnxruntime_c_api.h),-l:onnxruntime.dll,)
WINDRES    ?= $(WIN_HOST)-windres
WINRES     ?=                      # Windows resource object (set by win targets)
OSX_HOST   ?= o64               # osxcross clang wrapper: o64 = x86_64, oa64 = arm64
OSX_DEPS   ?=                    # darwin dep prefix: openssl + opus + srtp2 + usrsctp + pcap + ffmpeg + onnxruntime
# In-process depth on macOS: enabled when OSX_DEPS holds the onnxruntime headers (CoreML EP is
# built into the macOS ORT). The .dylib is shipped in the .app; rpath resolves it there.
OSX_ONNX_DEF  = $(if $(wildcard $(OSX_DEPS)/include/onnxruntime_c_api.h),-DBSDR_HAVE_ONNX=1,)
OSX_ONNX_LIBS = $(if $(wildcard $(OSX_DEPS)/include/onnxruntime_c_api.h),-lonnxruntime -Wl,-rpath,@executable_path/../Frameworks,)
OSX_BUILD  ?= build-osx          # per-arch override so x86_64/arm64 don't clobber
# Full macOS media stack (mirrors ./configure's macos branch): CoreAudio audio +
# owner-mic sniffer (libpcap) + SRTP + SCTP DataChannel + the RTP video sender +
# ffmpeg avfoundation/videotoolbox desktop capture. Derived from the canonical
# groups: the shared video+audio units + the macOS CoreAudio backend. sctp.c is
# supplied separately via SCTP_SRC in the osxcross recipe, so it's excluded here.
OSX_MEDIA_SRC  := $(MEDIA_SRC_VIDEO) $(MEDIA_SRC_AUDIO) src/audio_coreaudio.c src/macos_compat.c
OSX_MEDIA_DEF  := -DBSDR_ENABLE_SCTP=1 -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1 -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1 -DBSDR_HAVE_PCAP=1
# ffmpeg (avdevice/avfilter/avformat/avcodec/swscale/swresample/avutil) drives the
# avfoundation grab + h264_videotoolbox encode; the frameworks below are what those
# ffmpeg components link against on macOS. CoreGraphics/CoreFoundation are added by
# the osxcross recipe.
# Frameworks/libs are the full static-link set the bundled ffmpeg pulls in on macOS (from
# `pkg-config --libs --static libav*`): SecureTransport (Security), CoreImage/OpenGL filters,
# bz2/lzma/iconv, etc. CoreGraphics is added by the osxcross recipe.
OSX_MEDIA_LIBS := -lsrtp2 -lopus -lusrsctp -lpcap \
                  -lavdevice -lavfilter -lavformat -lavcodec -lswscale -lswresample -lavutil \
                  -lbz2 -lz -liconv -lm \
                  -framework CoreAudio -framework AudioToolbox -framework CoreFoundation \
                  -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
                  -framework AVFoundation -framework Foundation -framework CoreServices \
                  -framework OpenGL -framework CoreImage -framework AppKit -framework Security
# osxcross ships o64-clang/oa64-clang but NO o64-ar; ar/ranlib live under the full
# target triple only. Derive it from the compiler so we track the SDK's darwin
# version automatically (e.g. x86_64-apple-darwin25.1). Lazily expanded — the shell
# runs only for the osxcross target. NB: no trailing inline comment on the next line,
# or make would fold the whitespace before it into the value.
OSX_TARGET  = $(shell $(OSX_HOST)-clang -dumpmachine 2>/dev/null)
OSX_AR     ?= $(OSX_TARGET)-ar

# ---- artifacts -------------------------------------------------------------
CORE_SRC := src/log.c src/net.c src/json.c src/input_decode.c \
            src/discovery.c src/control.c src/udp_transport.c \
            src/dtls.c src/cloud.c src/cloud_stream.c src/app.c src/webui.c \
            src/overlay.c src/httpc.c src/tls.c src/stt.c src/llm.c \
            src/compcontrol.c src/voice.c src/screenshot.c src/threed.c \
            src/depth_onnx.c src/model_store.c src/webcam.c src/voicefx.c src/faceswap.c src/micsub.c \
            $(INJECT_SRC) $(WINLIST_SRC) $(SCTP_SRC) $(MEDIA_SRC)
# miniz (vendored, third_party) backs model_store.c's zip import; built in every config.
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC)) $(BUILD)/miniz.o

LIB   := $(BUILD)/libbsdr_core.a
AGENT := $(BUILD)/bsdr_agent$(EXEEXT)
TOOLS := $(BUILD)/dtls_probe$(EXEEXT) $(BUILD)/test_client$(EXEEXT)
# quest_sim links libsrtp (media) and is POSIX-only (sys/select.h): build it
# only where media is enabled and we're not cross-compiling for Windows.
ifneq ($(strip $(MEDIA_SRC)),)
ifneq ($(EXEEXT),.exe)
  TOOLS += $(BUILD)/quest_sim$(EXEEXT)
endif
endif
TESTS := $(BUILD)/test_input_decode$(EXEEXT) $(BUILD)/test_protocol$(EXEEXT) \
         $(BUILD)/test_transport$(EXEEXT) $(BUILD)/test_overlay$(EXEEXT) \
         $(BUILD)/test_compcontrol$(EXEEXT) $(BUILD)/test_voice$(EXEEXT)
ifneq ($(strip $(SCTP_SRC)),)
  TESTS += $(BUILD)/test_datachannel$(EXEEXT)
endif
ifneq ($(filter src/micsniff.c,$(MEDIA_SRC)),)   # owner-mic sniffer decode test (Linux audio only)
  TESTS += $(BUILD)/test_micsniff$(EXEEXT)
endif
ifeq ($(BUILD_TESTS),yes)
  ALL_TESTS := $(TESTS)
else
  ALL_TESTS :=
endif

.PHONY: all require-full-media linux windows windows-media osx osxcross check install uninstall clean distclean print-media-src
all: require-full-media $(AGENT) $(TOOLS) $(ALL_TESTS)
	@echo "bsdrX built -> $(BUILD)/  (CC=$(CC))"

# Full-feature guard: bsdrX is a remote-desktop agent, so desktop capture is
# mandatory on every platform — there is no core-only build. Native (autodetect)
# builds whose media deps are missing land here instead of silently producing a
# control-only agent; cross builds pass an explicit MEDIA_SRC (which contains
# capture.c) and pass straight through.
require-full-media:
	@case " $(MEDIA_SRC) " in \
	  *" src/capture.c "*) : ;; \
	  *) echo "ERROR: bsdrX is a full-feature build; desktop capture is required, but the media" >&2; \
	     echo "       deps were not found. Install libsrtp2, ffmpeg (libavcodec/avformat/avdevice/" >&2; \
	     echo "       avutil/swscale/swresample/avfilter), opus and libpulse-simple, then rebuild." >&2; \
	     exit 1 ;; \
	esac

# Emit the canonical full media source list (one file per line, sorted). This is
# the dependency-INDEPENDENT MEDIA_SRC_ALL, not the dep-autodetected HOST_MEDIA_SRC
# (which is empty inside the linux-bundle container where deps live in a private
# prefix). scripts/build-linux-bundle.sh asserts its hardcoded MEDIA_SRC against
# this. Keep it dependency-free.
print-media-src:
	@printf '%s\n' $(sort $(MEDIA_SRC_ALL))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(CONFIG_MK) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# vendored miniz (third-party): compile warning-free (-w), no bsdr includes needed.
# _LARGEFILE64_SOURCE makes glibc define __USE_LARGEFILE64, so miniz takes its
# fopen64/ftello64 large-file I/O path (miniz.h documents this) instead of the
# fallback branch that emits a "may not support large files" #pragma message.
$(BUILD)/miniz.o: third_party/miniz/miniz.c | $(BUILD)
	$(CC) $(CFLAGS) -D_LARGEFILE64_SOURCE=1 -w -c $< -o $@

$(LIB): $(CORE_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(AGENT): src/agent.c $(LIB) $(WINRES) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LIB) $(WINRES) $(LDFLAGS) $(LDLIBS)

# Windows resource (icon + version info); WINRES is set only by the win targets.
$(BUILD)/bsdrx_res.o: assets/bsdrx.rc assets/bsdrx.ico | $(BUILD)
	$(WINDRES) $< -O coff -o $@
$(BUILD)/%$(EXEEXT): tools/%.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LIB) $(LDFLAGS) $(LDLIBS)
$(BUILD)/%$(EXEEXT): tests/%.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LIB) $(LDFLAGS) $(LDLIBS)

# Router-side owner-mic relay: minimal deps (libpcap only) so it cross-compiles for OpenWRT etc.
# This specific rule overrides the generic tools pattern above so it does NOT link the core lib.
#   host build:  make micrelay
#   router:      CC=<router-toolchain-gcc> make micrelay   (needs libpcap headers/lib for the target)
.PHONY: micrelay
micrelay: $(BUILD)/bsdr_micrelay$(EXEEXT)
$(BUILD)/bsdr_micrelay$(EXEEXT): tools/bsdr_micrelay.c src/micsniff_capture.c | $(BUILD)
	$(CC) $(BASE_CFLAGS) -DBSDR_HAVE_PCAP=1 -Isrc $^ -lpcap -o $@

# ---- per-OS convenience targets (full media; cross-compile into own dirs) --
# Every target is full-feature (capture+encode, audio, SCTP, SRTP). There is no
# core-only build — see require-full-media above. Native `linux` autodetects the
# host media deps; `windows` and `osxcross` pin them through their *_DEPS prefix.
linux:
	$(MAKE) all BUILD=build-linux

# `windows` = the full media-capable mingw build (was `windows-media`). Kept as an
# alias so scripts/build-win-bundle.sh (make windows-media) keeps working.
windows: windows-media

# Fully media-capable Windows build (capture+encode, audio, LAN+cloud).
# Needs the mingw dep set from scripts/build-win-deps.sh in WIN_DEPS. FFmpeg is
# linked dynamically (its DLLs ship in the installer); our deps (opus/srtp2/
# usrsctp/openssl) are static .a, libgcc is static.
# Owner-mic sniffer on Windows: capture via Npcap (libpcap API; wpcap+Packet from the Npcap SDK in
# WIN_DEPS), virtual mic via VB-CABLE (audio_wasapi). Both need the agent run as Administrator.
# Derived from the canonical groups (sctp carried in-list; windows-media passes
# SCTP_SRC= empty) + the Windows WASAPI virtual-mic backend.
WIN_MEDIA_SRC := $(MEDIA_SRC_ALL) src/audio_wasapi.c
WIN_MEDIA_DEF := -DBSDR_ENABLE_SCTP=1 -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1 \
                 -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1 -DBSDR_HAVE_PCAP=1
WIN_MEDIA_LIBS := -lavdevice -lavfilter -lavformat -lavcodec -lswscale -lswresample -lavutil \
                  -lopus -lsrtp2 -lusrsctp -lssl -lcrypto -lwpcap -lPacket \
                  -lpthread -lws2_32 -liphlpapi -luser32 -lbcrypt -lcrypt32 -lgdi32 \
                  -lole32 -loleaut32 -luuid -lstrmiids -lwinmm -lksuser -lavrt -lmfplat -lmfuuid -lm
windows-media:
	@test -n "$(WIN_DEPS)" || { \
	  echo "usage: make windows-media WIN_DEPS=/path/to/win-deps  (run scripts/build-win-deps.sh first)"; exit 1; }
	$(MAKE) all BUILD=build-windows-media EXEEXT=.exe \
	  CC=$(WIN_HOST)-gcc AR=$(WIN_HOST)-ar INJECT_SRC=src/inject_win.c \
	  WINRES=build-windows-media/bsdrx_res.o WINDRES=$(WIN_HOST)-windres \
	  SCTP_SRC= WINLIST_SRC=src/winlist_win.c MEDIA_SRC="$(WIN_MEDIA_SRC)" \
	  CFLAGS="$(BASE_CFLAGS) -I$(WIN_DEPS)/include $(WIN_MEDIA_DEF) $(WIN_ONNX_DEF)" \
	  LDFLAGS="-static-libgcc -L$(WIN_DEPS)/lib" \
	  LDLIBS="-L$(WIN_DEPS)/lib $(WIN_MEDIA_LIBS) $(WIN_ONNX_LIBS)"

# Native macOS build (run this ON a Mac). Delegates to ./configure, which detects
# Darwin and wires the full CoreAudio + owner-mic-sniffer media stack (via Homebrew
# opus/libsrtp2/usrsctp + BlackHole), then builds. Re-run ./configure by hand to
# change options. To cross-compile from Linux instead, use 'make osxcross'.
osx:
	@[ "$$(uname -s)" = Darwin ] || { \
	  echo "make osx builds natively ON macOS. To cross-compile for macOS from Linux, use 'make osxcross' (needs osxcross)."; exit 1; }
	@test -f config.mk || ./configure
	+$(MAKE) all

# Cross-compile for macOS from Linux using the osxcross toolchain, feature-complete:
# CoreAudio audio + owner-mic sniffer (libpcap) + SRTP + SCTP DataChannel + the RTP
# video sender — the same stack the native 'osx' target builds (no desktop capture;
# macOS has no X11/NVENC grab). OSX_HOST picks the arch (o64 = x86_64, oa64 = arm64).
# OSX_DEPS is a darwin prefix holding openssl+opus+srtp2+usrsctp+pcap static libs.
osxcross:
	@test -n "$(OSX_DEPS)" || { \
	  echo "usage: make osxcross OSX_DEPS=/path/to/darwin-deps [OSX_HOST=o64|oa64] [OSX_BUILD=$(OSX_BUILD)]"; \
	  echo "  OSX_DEPS = a prefix with darwin static libs: openssl + opus + srtp2 + usrsctp + pcap + ffmpeg. Needs osxcross in PATH."; exit 1; }
	@command -v $(OSX_HOST)-clang >/dev/null 2>&1 || { echo "error: $(OSX_HOST)-clang not in PATH (osxcross not installed?)"; exit 1; }
	$(MAKE) all BUILD=$(OSX_BUILD) EXEEXT= \
	  MEDIA_SRC="$(OSX_MEDIA_SRC)" SCTP_SRC=src/sctp.c WINLIST_SRC=src/winlist_null.c \
	  CC=$(OSX_HOST)-clang AR="$(OSX_AR)" INJECT_SRC=src/inject_macos.c \
	  CFLAGS="$(BASE_CFLAGS) $(OSX_MEDIA_DEF) $(OSX_ONNX_DEF) -I$(OSX_DEPS)/include" \
	  LDLIBS="-L$(OSX_DEPS)/lib $(OSX_MEDIA_LIBS) $(OSX_ONNX_LIBS) -lssl -lcrypto -framework CoreGraphics -framework CoreFoundation"

# ---- run tests (RUNNER lets you prefix e.g. wine for cross builds) ----------
check: $(ALL_TESTS)
	@status=0; for t in $(ALL_TESTS); do \
	  echo "== $$t =="; $(RUNNER) ./$$t || status=1; done; \
	exit $$status

install: all
	mkdir -p "$(DESTDIR)$(bindir)" "$(DESTDIR)$(libdir)" "$(DESTDIR)$(includedir)/bsdr" \
	         "$(DESTDIR)$(mandir)/man1"
	cp -p $(AGENT) $(TOOLS) "$(DESTDIR)$(bindir)/"
	cp -p $(LIB) "$(DESTDIR)$(libdir)/"
	cp -p include/bsdr/*.h "$(DESTDIR)$(includedir)/bsdr/"
	cp -p docs/bsdr_agent.1 docs/bsdr_micrelay.1 "$(DESTDIR)$(mandir)/man1/"
	@echo "installed to $(DESTDIR)$(prefix)"

uninstall:
	rm -f "$(DESTDIR)$(bindir)/bsdr_agent$(EXEEXT)" \
	      "$(DESTDIR)$(bindir)/dtls_probe$(EXEEXT)" \
	      "$(DESTDIR)$(bindir)/test_client$(EXEEXT)" \
	      "$(DESTDIR)$(libdir)/libbsdr_core.a" \
	      "$(DESTDIR)$(mandir)/man1/bsdr_agent.1" \
	      "$(DESTDIR)$(mandir)/man1/bsdr_micrelay.1"
	rm -rf "$(DESTDIR)$(includedir)/bsdr"

clean:
	rm -rf build build-linux build-linux-static build-windows build-osx

distclean: clean
	rm -f config.mk
