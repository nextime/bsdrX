# bsdrX Makefile — works two ways:
#
#   ./configure && make           configured host build (honors config.mk)
#   make                          build for the local OS (no configure needed)
#   make linux                    build for Linux            -> build-linux/
#   make linux-static             static Linux binary        -> build-linux-static/
#   make windows WIN_OPENSSL=DIR  cross-build for Windows     -> build-windows/
#   make osx                      native build ON macOS (full media via configure)
#   make osxcross OSX_DEPS=DIR    cross-build macOS from Linux, full media (osxcross) -> build-osx/
#
#   (for a static build WITH media, use ./configure --static && make)
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
BASE_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude

# ---- host defaults (used only when config.mk did not set them) -------------
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  HOST_INJECT   := src/inject_macos.c
  HOST_PLATLIBS := -framework CoreGraphics -framework CoreFoundation
else
  HOST_INJECT   := src/inject_linux.c
  HOST_PLATLIBS := -lpthread
endif
HOST_OSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
HOST_OSSL_LIBS   := $(shell pkg-config --libs openssl 2>/dev/null)
ifeq ($(strip $(HOST_OSSL_LIBS)),)
  HOST_OSSL_LIBS := -lssl -lcrypto
endif

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
    HOST_MEDIA_SRC += src/sctp.c
    HOST_MEDIA_DEF += -DBSDR_ENABLE_SCTP=1
    HOST_MEDIA_LIBS += -lusrsctp
  endif
  ifneq ($(shell pkg-config --exists libsrtp2 libavcodec libavformat libavdevice libswscale libswresample 2>/dev/null && echo 1),)
    HOST_MEDIA_SRC += src/srtp_util.c src/video.c src/capture.c src/threed.c src/filesrc.c src/fileaudio.c
    HOST_MEDIA_DEF += -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1
    HOST_MEDIA_CFLAGS += $(shell pkg-config --cflags libsrtp2 libavcodec libavformat libavdevice libavutil libswscale libswresample 2>/dev/null)
    HOST_MEDIA_LIBS += $(shell pkg-config --libs libsrtp2 libavcodec libavformat libavdevice libavutil libswscale libswresample 2>/dev/null) -lX11
    HOST_WINLIST := src/winlist.c              # X11 window list (needs -lX11 above)
  endif
  ifneq ($(shell pkg-config --exists opus libpulse-simple 2>/dev/null && echo 1),)
    HOST_MEDIA_SRC += src/audio.c src/micsniff.c src/micsniff_capture.c   # owner-mic sniffer
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

CC          ?= cc
AR          ?= ar
EXEEXT      ?=
BUILD       ?= build
INJECT_SRC  ?= $(HOST_INJECT)
WINLIST_SRC ?= $(HOST_WINLIST)
SCTP_SRC    ?=
MEDIA_SRC   ?= $(HOST_MEDIA_SRC)
CFLAGS      ?= $(BASE_CFLAGS) $(HOST_OSSL_CFLAGS) $(HOST_MEDIA_DEF) $(HOST_MEDIA_CFLAGS)
LDFLAGS     ?=
LDLIBS      ?= $(HOST_OSSL_LIBS) $(HOST_PLATLIBS) $(HOST_MEDIA_LIBS)
BUILD_TESTS ?= yes
prefix      ?= /usr/local
bindir      ?= $(prefix)/bin
libdir      ?= $(prefix)/lib
includedir  ?= $(prefix)/include
mandir      ?= $(prefix)/share/man
RUNNER      ?=

# cross-target knobs
WIN_HOST   ?= x86_64-w64-mingw32
WIN_OPENSSL ?=
WIN_DEPS   ?=                      # unified mingw prefix from scripts/build-win-deps.sh
WINDRES    ?= $(WIN_HOST)-windres
WINRES     ?=                      # Windows resource object (set by win targets)
WIN_LIBDIR := $(if $(wildcard $(WIN_OPENSSL)/lib64/libssl.a),lib64,lib)
OSX_HOST   ?= o64               # osxcross clang wrapper: o64 = x86_64, oa64 = arm64
OSX_DEPS   ?=                    # darwin dep prefix: openssl + opus + srtp2 + usrsctp + pcap
OSX_BUILD  ?= build-osx          # per-arch override so x86_64/arm64 don't clobber
# Full macOS media stack (mirrors ./configure's macos branch): CoreAudio audio +
# owner-mic sniffer (libpcap) + SRTP + SCTP DataChannel + the RTP video sender.
# No capture.c / BSDR_HAVE_CAPTURE — macOS has no X11/NVENC desktop grab.
OSX_MEDIA_SRC  := src/srtp_util.c src/video.c src/audio.c src/audio_coreaudio.c src/micsniff.c src/micsniff_capture.c
OSX_MEDIA_DEF  := -DBSDR_ENABLE_SCTP=1 -DBSDR_ENABLE_VIDEO=1 -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1 -DBSDR_HAVE_PCAP=1
OSX_MEDIA_LIBS := -lsrtp2 -lopus -lusrsctp -lpcap -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
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
            src/compcontrol.c src/voice.c src/screenshot.c \
            $(INJECT_SRC) $(WINLIST_SRC) $(SCTP_SRC) $(MEDIA_SRC)
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC))

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

.PHONY: all linux linux-static windows windows-media osx osxcross check install uninstall clean distclean
all: $(AGENT) $(TOOLS) $(ALL_TESTS)
	@echo "bsdrX built -> $(BUILD)/  (CC=$(CC))"

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(CONFIG_MK) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

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

# ---- per-OS convenience targets (cross-compile into their own dirs) --------
linux:
	$(MAKE) all BUILD=build-linux EXEEXT= INJECT_SRC=src/inject_linux.c \
	  WINLIST_SRC=src/winlist_null.c MEDIA_SRC= SCTP_SRC= \
	  CFLAGS="$(BASE_CFLAGS) $(HOST_OSSL_CFLAGS)" \
	  LDLIBS="$(HOST_OSSL_LIBS) -lpthread"

# static, self-contained core binary (no media; needs static OpenSSL .a)
LINUX_STATIC_OSSL := $(shell pkg-config --static --libs openssl 2>/dev/null)
ifeq ($(strip $(LINUX_STATIC_OSSL)),)
  LINUX_STATIC_OSSL := -lssl -lcrypto
endif
linux-static:
	$(MAKE) all BUILD=build-linux-static EXEEXT= INJECT_SRC=src/inject_linux.c \
	  WINLIST_SRC=src/winlist_null.c MEDIA_SRC= SCTP_SRC= \
	  CFLAGS="$(BASE_CFLAGS) $(HOST_OSSL_CFLAGS)" \
	  LDFLAGS="-static" \
	  LDLIBS="$(LINUX_STATIC_OSSL) -lpthread"

windows:
	@test -n "$(WIN_OPENSSL)" || { \
	  echo "usage: make windows WIN_OPENSSL=/path/to/mingw-openssl [WIN_HOST=$(WIN_HOST)]"; exit 1; }
	$(MAKE) all BUILD=build-windows EXEEXT=.exe \
	  MEDIA_SRC= SCTP_SRC= WINLIST_SRC=src/winlist_win.c \
	  CC=$(WIN_HOST)-gcc AR=$(WIN_HOST)-ar INJECT_SRC=src/inject_win.c \
	  WINRES=build-windows/bsdrx_res.o WINDRES=$(WIN_HOST)-windres \
	  CFLAGS="$(BASE_CFLAGS) -I$(WIN_OPENSSL)/include" \
	  LDFLAGS="-static -static-libgcc" \
	  LDLIBS="-L$(WIN_OPENSSL)/$(WIN_LIBDIR) -lssl -lcrypto -lws2_32 -luser32 -lbcrypt -lcrypt32"

# Fully media-capable Windows build (capture+encode, audio, LAN+cloud).
# Needs the mingw dep set from scripts/build-win-deps.sh in WIN_DEPS. FFmpeg is
# linked dynamically (its DLLs ship in the installer); our deps (opus/srtp2/
# usrsctp/openssl) are static .a, libgcc is static.
# Owner-mic sniffer on Windows: capture via Npcap (libpcap API; wpcap+Packet from the Npcap SDK in
# WIN_DEPS), virtual mic via VB-CABLE (audio_wasapi). Both need the agent run as Administrator.
WIN_MEDIA_SRC := src/sctp.c src/srtp_util.c src/video.c src/capture.c src/threed.c src/filesrc.c src/fileaudio.c \
                 src/audio.c src/audio_wasapi.c src/micsniff.c src/micsniff_capture.c
WIN_MEDIA_DEF := -DBSDR_ENABLE_SCTP=1 -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1 \
                 -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1 -DBSDR_HAVE_PCAP=1
WIN_MEDIA_LIBS := -lavdevice -lavfilter -lavformat -lavcodec -lswscale -lswresample -lavutil \
                  -lopus -lsrtp2 -lusrsctp -lssl -lcrypto -lwpcap -lPacket \
                  -lpthread -lws2_32 -liphlpapi -luser32 -lbcrypt -lcrypt32 -lgdi32 \
                  -lole32 -loleaut32 -luuid -lwinmm -lksuser -lavrt -lmfplat -lmfuuid
windows-media:
	@test -n "$(WIN_DEPS)" || { \
	  echo "usage: make windows-media WIN_DEPS=/path/to/win-deps  (run scripts/build-win-deps.sh first)"; exit 1; }
	$(MAKE) all BUILD=build-windows-media EXEEXT=.exe \
	  CC=$(WIN_HOST)-gcc AR=$(WIN_HOST)-ar INJECT_SRC=src/inject_win.c \
	  WINRES=build-windows-media/bsdrx_res.o WINDRES=$(WIN_HOST)-windres \
	  SCTP_SRC= WINLIST_SRC=src/winlist_win.c MEDIA_SRC="$(WIN_MEDIA_SRC)" \
	  CFLAGS="$(BASE_CFLAGS) -I$(WIN_DEPS)/include $(WIN_MEDIA_DEF)" \
	  LDFLAGS="-static-libgcc -L$(WIN_DEPS)/lib" \
	  LDLIBS="-L$(WIN_DEPS)/lib $(WIN_MEDIA_LIBS)"

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
	  echo "  OSX_DEPS = a prefix with darwin static libs: openssl + opus + srtp2 + usrsctp + pcap. Needs osxcross in PATH."; exit 1; }
	@command -v $(OSX_HOST)-clang >/dev/null 2>&1 || { echo "error: $(OSX_HOST)-clang not in PATH (osxcross not installed?)"; exit 1; }
	$(MAKE) all BUILD=$(OSX_BUILD) EXEEXT= \
	  MEDIA_SRC="$(OSX_MEDIA_SRC)" SCTP_SRC=src/sctp.c WINLIST_SRC=src/winlist_null.c \
	  CC=$(OSX_HOST)-clang AR="$(OSX_AR)" INJECT_SRC=src/inject_macos.c \
	  CFLAGS="$(BASE_CFLAGS) $(OSX_MEDIA_DEF) -I$(OSX_DEPS)/include" \
	  LDLIBS="-L$(OSX_DEPS)/lib $(OSX_MEDIA_LIBS) -lssl -lcrypto -framework CoreGraphics -framework CoreFoundation"

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
