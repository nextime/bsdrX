#!/usr/bin/env bash
# build-linux-bundle.sh — produce a distributable Linux bundle for bsdrX:
#   * a portable AppImage (runs across distros; bundles a MINIMAL ffmpeg + media deps)
#   * a .deb package (installs to /opt/bsdrX + a /usr/bin symlink + the uinput udev rule)
#   * bsdrX.zip containing both, plus README.md and LICENSE.md
#
# Designed to run inside the bsdrx-linux-deps build image (Ubuntu 20.04 = glibc 2.31,
# with a private minimal ffmpeg + openssl3/x264/opus/srtp2/usrsctp/pcap under
# $BSDRX_DEPS). See scripts/linux-bundle.Dockerfile. Example:
#
#   docker build -f scripts/linux-bundle.Dockerfile -t bsdrx-linux-deps scripts
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out bsdrx-linux-deps \
#           bash /src/scripts/build-linux-bundle.sh
#
# Env: BSDRX_DEPS (default /opt/bsdrx-deps), SRC (default /src), OUT (default /out),
#      VERSION (default: git describe or "0.0.0").
set -euo pipefail

BSDRX_DEPS="${BSDRX_DEPS:-/opt/bsdrx-deps}"
SRC="${SRC:-/src}"
OUT="${OUT:-/out}"
WORK="$(mktemp -d)"
ARCH="$(uname -m)"                       # x86_64
export LD_LIBRARY_PATH="$BSDRX_DEPS/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$BSDRX_DEPS/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PATH="$BSDRX_DEPS/bin:$PATH"

VERSION="${VERSION:-}"
if [ -z "$VERSION" ]; then
  VERSION="$(sed -n 's/.*BSDR_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/include/bsdr/version.h" 2>/dev/null)"
  [ -n "$VERSION" ] || VERSION="$(git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)"
fi
echo ">> bsdrX linux bundle  version=$VERSION  arch=$ARCH  deps=$BSDRX_DEPS"

mkdir -p "$OUT"
cp -a "$SRC" "$WORK/bsdrX"
cd "$WORK/bsdrX"
make clean >/dev/null 2>&1 || true

# ---- 1. build bsdrX (full media) against the private minimal-ffmpeg prefix ----
# Explicit vars (not ./configure autodetect) so we pin OUR deps and set an rpath
# that finds the bundled libs at $ORIGIN/../lib.
BASE_CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -Ithird_party/miniz -Ithird_party/minimp3 -Ithird_party/cjson"
MEDIA_SRC="src/srtp_util.c src/video.c src/capture.c src/filesrc.c src/fileaudio.c src/capture_pipewire.c src/term.c src/audio.c src/micsniff.c src/micsniff_capture.c"
MEDIA_DEF="-DBSDR_ENABLE_SCTP=1 -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1 -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1 -DBSDR_HAVE_PCAP=1"
# Wayland desktop capture (xdg-desktop-portal + PipeWire). System libs in the build image; linuxdeploy
# bundles them like the others. Absent -> capture_pipewire.c compiles as a stub (x11grab/kmsgrab only).
if pkg-config --exists libpipewire-0.3 dbus-1 2>/dev/null; then
  MEDIA_DEF="$MEDIA_DEF -DBSDR_HAVE_PIPEWIRE=1"
  PW_CFLAGS="$(pkg-config --cflags libpipewire-0.3 dbus-1)"
  PW_LIBS="$(pkg-config --libs libpipewire-0.3 dbus-1)"
else
  PW_CFLAGS=""; PW_LIBS=""
  echo ">> WARN: libpipewire-0.3/dbus-1 not in the build image — Wayland capture will be stubbed"
fi

# Guard: the list above is a hand-pinned copy of the Makefile's canonical media
# list (MEDIA_SRC_ALL). If a new media unit is added there but not here, the
# bundle fails to link (as threed.c/fileaudio.c once did). Assert they agree,
# treating sctp.c specially since we pass it as SCTP_SRC (below), not in
# MEDIA_SRC. `make print-media-src` emits MEDIA_SRC_ALL, which is dependency-
# INDEPENDENT — so this holds even in this container where the deps live under a
# private prefix and pkg-config autodetection would come up empty.
want="$(make -s print-media-src)"
have="$(printf '%s\n' $MEDIA_SRC src/sctp.c | LC_ALL=C sort)"
if [ "$want" != "$have" ]; then
  echo "ERROR: bundle MEDIA_SRC has drifted from the Makefile's MEDIA_SRC_ALL." >&2
  echo "  Reconcile scripts/build-linux-bundle.sh with the Makefile (diff want-vs-have):" >&2
  diff <(printf '%s\n' "$want") <(printf '%s\n' "$have") >&2 || true
  exit 1
fi
MEDIA_LIBS="-lusrsctp -lsrtp2 -lavdevice -lavfilter -lavformat -lavcodec -lavutil -lswscale -lswresample -lopus -lpulse-simple -lpulse -lpcap -lX11 $PW_LIBS -lssl -lcrypto -lpthread -lm"
# in-process depth via ONNX Runtime (shipped in the bundle's lib dir; $ORIGIN/../lib rpath finds it)
if [ -f "$BSDRX_DEPS/include/onnxruntime_c_api.h" ]; then
  MEDIA_DEF="$MEDIA_DEF -DBSDR_HAVE_ONNX=1"; MEDIA_LIBS="$MEDIA_LIBS -lonnxruntime"
fi

# Wayland privacy screen-blank (wlr-gamma-control, wlroots compositors). Needs wayland-scanner +
# libwayland-client in the build image; the Makefile codegens the client glue from the vendored XML.
# We pass WAYLAND_GAMMA explicitly either way: this build sets its own vars instead of ./configure, so a
# stale config.mk from a prior ./configure must NOT leak WAYLAND_GAMMA=1 into an image without the scanner
# (that's exactly what tripped "wayland-scanner: Command not found"). Absent -> X11 xrandr screen-blank only.
WAYLAND_GAMMA=
WLG_SRC=""; WLG_CFLAGS=""; WLG_LIBS=""
if command -v wayland-scanner >/dev/null 2>&1 && pkg-config --exists wayland-client 2>/dev/null; then
  WAYLAND_GAMMA=1
  WLG_SRC="src/screenblank_wayland.c"
  MEDIA_DEF="$MEDIA_DEF -DBSDR_HAVE_WAYLAND_GAMMA=1"
  # screenblank_wayland.c includes the wayland-scanner-generated header the Makefile emits into the
  # build dir (BUILD=build-bundle). The Makefile adds -I$(BUILD) itself, but our command-line CFLAGS
  # override wins, so add it here too or the generated header isn't found.
  WLG_CFLAGS="$(pkg-config --cflags wayland-client) -Ibuild-bundle"
  WLG_LIBS="$(pkg-config --libs wayland-client)"
  echo ">> Wayland privacy screen-blank = wlr-gamma-control"
else
  echo ">> WARN: wayland-scanner/libwayland-client not in the build image — Wayland screen-blank stubbed (X11 xrandr only)"
fi

# Terminal source: PTY backend (vendored libvterm, no X) + XVFB backend (XTEST injection). term.c is
# in MEDIA_SRC above (asserted); term_pty.c + the libvterm objects are passed separately (they are not
# in the Makefile's canonical MEDIA_SRC_ALL, so they must not enter the drift assertion). libvterm is
# vendored under third_party, so the PTY backend always builds; XTEST needs libXtst in the image.
HAVE_VTERM=; VTERM_SRC=""; VTERM_LIBS=""
if [ -f third_party/libvterm/src/vterm.c ]; then
  HAVE_VTERM=1; VTERM_SRC="src/term_pty.c"; VTERM_LIBS="-lutil"
  MEDIA_DEF="$MEDIA_DEF -DBSDR_HAVE_VTERM=1 -Ithird_party/libvterm/include"
  echo ">> terminal PTY backend = vendored libvterm 0.3.3"
fi
XTST_LIBS=""
if pkg-config --exists xtst 2>/dev/null || [ -f /usr/include/X11/extensions/XTest.h ]; then
  MEDIA_DEF="$MEDIA_DEF -DBSDR_HAVE_XTEST=1"; XTST_LIBS="$(pkg-config --libs xtst 2>/dev/null || echo -lXtst)"
  echo ">> terminal XVFB backend / Unicode keys = X11 XTEST"
fi

make all BUILD=build-bundle EXEEXT= BUILD_TESTS=no WAYLAND_GAMMA="$WAYLAND_GAMMA" HAVE_VTERM="$HAVE_VTERM" \
  INJECT_SRC=src/inject_linux.c WINLIST_SRC=src/winlist.c \
  MEDIA_SRC="$MEDIA_SRC $WLG_SRC $VTERM_SRC" SCTP_SRC=src/sctp.c \
  CFLAGS="$BASE_CFLAGS $MEDIA_DEF $PW_CFLAGS $WLG_CFLAGS -I$BSDRX_DEPS/include" \
  LDFLAGS="-L$BSDRX_DEPS/lib -Wl,-rpath,\$\$ORIGIN/../lib -Wl,--disable-new-dtags" \
  LDLIBS="$MEDIA_LIBS $WLG_LIBS $VTERM_LIBS $XTST_LIBS -ldl"   # -ldl: src/plugin.c dlopen()s plugins (older glibc keeps it in libdl)

BIN="build-bundle/bsdr_agent"
test -x "$BIN"
strip -s "$BIN"                          # strip symbols from the agent binary only (not the libs)
echo ">> built + stripped $BIN"

# ---- 2. assemble an AppDir, bundle deps, build the AppImage --------------------
APPDIR="$WORK/AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$BIN" "$APPDIR/usr/bin/bsdr_agent"
cp assets/bsdrx-256.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/bsdrX.png"

# Reverse-DNS basenames throughout (desktop id, metainfo filename, AppStream <id> and
# the launchable that ties them together). This is the modern freedesktop/AppStream
# convention and it's what keeps appimagetool's bundled appstreamcli happy: a bare
# "bsdrX" id trips cid-desktopapp-is-not-rdns, while a reverse-DNS id in a "bsdrX"-named
# file trips metainfo-filename-cid-mismatch — only when all three agree do both clear.
APPID="net.nexlab.bsdrX"
cat > "$APPDIR/usr/share/applications/$APPID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=bsdrX
Comment=Bigscreen Remote Desktop agent (cast this desktop into a VR headset)
Exec=bsdr_agent
Icon=bsdrX
Categories=Network;RemoteAccess;
Terminal=true
EOF

# AppStream metainfo — appimagetool looks for usr/share/metainfo/<desktop-id>.appdata.xml
# and warns if it's absent or fails validation. RELDATE stays build-stable via VERSION.
RELDATE="$(date -u +%Y-%m-%d 2>/dev/null || echo 2024-01-01)"
mkdir -p "$APPDIR/usr/share/metainfo"
cat > "$APPDIR/usr/share/metainfo/$APPID.appdata.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>net.nexlab.bsdrX</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>GPL-3.0-or-later</project_license>
  <name>bsdrX</name>
  <summary>Cast a Linux desktop into a Bigscreen VR headset</summary>
  <description>
    <p>
      bsdrX is a Bigscreen Remote Desktop agent for Linux. It casts a desktop into a
      Bigscreen VR headset over the LAN with H.264 video and two-way Opus audio, injects
      headset input via uinput, and provides an in-VR control bar and a voice assistant.
    </p>
  </description>
  <launchable type="desktop-id">net.nexlab.bsdrX.desktop</launchable>
  <url type="homepage">https://git.nexlab.net/nextime/bsdrX</url>
  <developer_name>Stefy Lanza (nexlab)</developer_name>
  <content_rating type="oars-1.1"/>
  <releases>
    <release version="${VERSION#v}" date="${RELDATE}"/>
  </releases>
</component>
EOF

# Self-contained PipeWire (Wayland capture): linuxdeploy bundles libpipewire-0.3.so.0 (an ldd dep),
# but at runtime that client dlopens its SPA plugins + protocol modules from a compiled-in path — on
# the TARGET those would be a different PipeWire (maybe 1.x) and mismatch the bundled 0.3.x client. So
# bundle the version-matched spa-0.2 + pipewire-0.3 trees and point libpipewire at them via an AppRun
# hook (linuxdeploy's AppRun exports $APPDIR and sources apprun-hooks/*.sh). The client then stays
# internally consistent and only talks to the target's daemon over the socket (protocol-stable).
if [ -n "$PW_LIBS" ]; then
  PW_MODDIR="$(pkg-config --variable=moduledir libpipewire-0.3 2>/dev/null)"
  SPA_DIR="$([ -n "$PW_MODDIR" ] && echo "$(dirname "$PW_MODDIR")/spa-0.2")"
  if [ -d "$PW_MODDIR" ] && [ -d "$SPA_DIR" ]; then
    mkdir -p "$APPDIR/usr/lib/pipewire-0.3" "$APPDIR/usr/lib/spa-0.2" "$APPDIR/apprun-hooks"
    cp -a "$PW_MODDIR"/. "$APPDIR/usr/lib/pipewire-0.3/"
    cp -a "$SPA_DIR"/.   "$APPDIR/usr/lib/spa-0.2/"
    # Bundle libpipewire-0.3.so.* itself too: it's a DT_NEEDED of the binary but linuxdeploy's
    # excludelist skips it, which would leave the target's (maybe 1.x) client loading our 0.3.x
    # plugins — a mismatch. Placing it in usr/lib (on the AppRun's LD_LIBRARY_PATH) keeps the whole
    # PipeWire stack self-consistent at 0.3.x.
    cp -a "$(dirname "$PW_MODDIR")"/libpipewire-0.3.so* "$APPDIR/usr/lib/" 2>/dev/null || \
      echo ">> WARN: libpipewire-0.3.so not found next to $PW_MODDIR — client may fall back to the target's"
    cat > "$APPDIR/apprun-hooks/pipewire.sh" <<'HOOK'
# Point the bundled libpipewire at its OWN (version-matched) spa plugins + modules, not the target's.
export SPA_PLUGIN_DIR="${APPDIR}/usr/lib/spa-0.2"
export PIPEWIRE_MODULE_DIR="${APPDIR}/usr/lib/pipewire-0.3"
HOOK
    echo ">> bundled PipeWire self-contained (spa-0.2 + pipewire-0.3 modules + AppRun hook)"
  else
    echo ">> WARN: PipeWire plugin/module dirs not found ($PW_MODDIR / $SPA_DIR) — Wayland capture may fail on newer targets"
  fi
fi

# linuxdeploy follows ldd (honoring LD_LIBRARY_PATH) and copies non-system libs,
# skipping glibc / GPU-driver libs via its built-in exclude list. FUSE-less mode.
export APPIMAGE_EXTRACT_AND_RUN=1
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"   # non-deprecated replacement for $VERSION
LD=/opt/tools/linuxdeploy-x86_64.AppImage
# Run with VERSION unset so the appimage plugin uses LINUXDEPLOY_OUTPUT_VERSION and
# doesn't print the "$VERSION is deprecated" warning.
env -u VERSION "$LD" --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/bsdr_agent" \
  --desktop-file "$APPDIR/usr/share/applications/$APPID.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/bsdrX.png" \
  --output appimage
mv -f bsdrX*.AppImage "$OUT/bsdrX-${ARCH}.AppImage" 2>/dev/null || \
  mv -f ./*.AppImage "$OUT/bsdrX-${ARCH}.AppImage"
chmod +x "$OUT/bsdrX-${ARCH}.AppImage"
echo ">> AppImage -> $OUT/bsdrX-${ARCH}.AppImage"

# ---- 3. .deb: reuse the libs linuxdeploy already resolved into the AppDir ------
DEBARCH="$(dpkg --print-architecture)"        # amd64
PKG="$WORK/deb"
rm -rf "$PKG"
mkdir -p "$PKG/DEBIAN" "$PKG/opt/bsdrX/bin" "$PKG/opt/bsdrX/lib" \
         "$PKG/usr/bin" "$PKG/lib/udev/rules.d" "$PKG/usr/share/doc/bsdr-agent"
cp "$BIN" "$PKG/opt/bsdrX/bin/bsdr_agent"
# bundled shared libs (everything linuxdeploy pulled in, minus the ELF loader)
find "$APPDIR/usr/lib" -maxdepth 1 -type f -name '*.so*' -exec cp -a {} "$PKG/opt/bsdrX/lib/" \;
find "$APPDIR/usr/lib" -maxdepth 1 -type l -name '*.so*' -exec cp -a {} "$PKG/opt/bsdrX/lib/" \;
# self-contained PipeWire in the .deb too: copy the bundled spa-0.2 + pipewire-0.3 trees (mirrors the AppImage)
for d in spa-0.2 pipewire-0.3; do [ -d "$APPDIR/usr/lib/$d" ] && cp -a "$APPDIR/usr/lib/$d" "$PKG/opt/bsdrX/lib/"; done
patchelf --set-rpath '$ORIGIN/../lib' "$PKG/opt/bsdrX/bin/bsdr_agent"
# /usr/bin launcher: a wrapper (not a bare symlink) so it can point the bundled libpipewire at the
# bundled plugin trees, matching the AppImage's AppRun hook. Falls through harmlessly if absent.
cat > "$PKG/usr/bin/bsdr_agent" <<'WRAP'
#!/bin/sh
[ -d /opt/bsdrX/lib/spa-0.2 ]     && export SPA_PLUGIN_DIR=/opt/bsdrX/lib/spa-0.2
[ -d /opt/bsdrX/lib/pipewire-0.3 ] && export PIPEWIRE_MODULE_DIR=/opt/bsdrX/lib/pipewire-0.3
exec /opt/bsdrX/bin/bsdr_agent "$@"
WRAP
chmod +x "$PKG/usr/bin/bsdr_agent"

cp assets/../docs/../README.md "$PKG/usr/share/doc/bsdr-agent/README.md" 2>/dev/null || cp README.md "$PKG/usr/share/doc/bsdr-agent/"
cp LICENSE.md "$PKG/usr/share/doc/bsdr-agent/"
# ship the AppStream metainfo in the .deb too (same file the AppImage embeds)
mkdir -p "$PKG/usr/share/metainfo"
cp "$APPDIR/usr/share/metainfo/$APPID.appdata.xml" "$PKG/usr/share/metainfo/$APPID.appdata.xml"
cat > "$PKG/lib/udev/rules.d/99-uinput.rules" <<'EOF'
KERNEL=="uinput", GROUP="uinput", MODE="0660"
EOF

INSTALLED_KB=$(du -sk "$PKG/opt" "$PKG/usr" "$PKG/lib" | awk '{s+=$1} END{print s}')
cat > "$PKG/DEBIAN/control" <<EOF
Package: bsdr-agent
Version: ${VERSION#v}
Section: net
Priority: optional
Architecture: ${DEBARCH}
Maintainer: Stefy Lanza <stefy@nexlab.net>
Installed-Size: ${INSTALLED_KB}
Depends: libc6 (>= 2.31), libx11-6, libxcb1, libpulse0
Description: Bigscreen Remote Desktop agent (desktop-in-VR)
 Casts a Linux desktop into a Bigscreen VR headset over the LAN: H.264 video,
 two-way Opus audio, headset input injected via uinput, an in-VR control bar and
 a voice assistant. Ships a private minimal ffmpeg + media libraries under
 /opt/bsdrX/lib, so it depends only on the base system + X11 + PulseAudio.
EOF

cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
getent group uinput >/dev/null 2>&1 || groupadd -f uinput || true
if command -v udevadm >/dev/null 2>&1; then udevadm control --reload 2>/dev/null || true; fi
exit 0
EOF
chmod 0755 "$PKG/DEBIAN/postinst"

dpkg-deb --build --root-owner-group "$PKG" "$OUT/bsdr-agent_${VERSION#v}_${DEBARCH}.deb"
DEB="$(ls -1 "$OUT"/bsdr-agent_*_"${DEBARCH}".deb | tail -1)"
echo ">> deb -> $DEB"

# ---- 4. zip everything for distribution --------------------------------------
ZIPTMP="$WORK/zip"; mkdir -p "$ZIPTMP"
cp "$OUT/bsdrX-${ARCH}.AppImage" "$ZIPTMP/"
cp "$DEB" "$ZIPTMP/"
cp "$SRC/README.md" "$SRC/LICENSE.md" "$ZIPTMP/"
( cd "$ZIPTMP" && zip -q -r "$OUT/bsdrX.zip" . )
echo ">> bundle -> $OUT/bsdrX.zip"
( cd "$OUT" && ls -la bsdrX.zip bsdrX-${ARCH}.AppImage bsdr-agent_*_${DEBARCH}.deb )

# ---- 5. loadable plugins (private; NOT inside the bundle) — packaged on their own, per platform ----
# The AppImage/.deb above intentionally ship NO plugin; here we build each plugins/<name>/ with the
# container's native toolchain (Linux/$ARCH) and emit one zip per plugin. No-op if there's no plugins/.
SRC="$WORK/bsdrX" OUT="$OUT" VERSION="$VERSION" PLATFORM=linux ARCH="$ARCH" PLUGIN_CC=cc \
  bash "$WORK/bsdrX/scripts/build-plugins.sh" || echo ">> WARN: plugin packaging failed (non-fatal)"

rm -rf "$WORK"
