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
BASE_CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude"
MEDIA_SRC="src/srtp_util.c src/video.c src/capture.c src/filesrc.c src/fileaudio.c src/audio.c src/micsniff.c src/micsniff_capture.c"
MEDIA_DEF="-DBSDR_ENABLE_SCTP=1 -DBSDR_ENABLE_VIDEO=1 -DBSDR_HAVE_CAPTURE=1 -DBSDR_ENABLE_AUDIO=1 -DBSDR_HAVE_AUDIO=1 -DBSDR_HAVE_PCAP=1"

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
MEDIA_LIBS="-lusrsctp -lsrtp2 -lavdevice -lavfilter -lavformat -lavcodec -lavutil -lswscale -lswresample -lopus -lpulse-simple -lpulse -lpcap -lX11 -lssl -lcrypto -lpthread"
# in-process depth via ONNX Runtime (shipped in the bundle's lib dir; $ORIGIN/../lib rpath finds it)
if [ -f "$BSDRX_DEPS/include/onnxruntime_c_api.h" ]; then
  MEDIA_DEF="$MEDIA_DEF -DBSDR_HAVE_ONNX=1"; MEDIA_LIBS="$MEDIA_LIBS -lonnxruntime"
fi

make all BUILD=build-bundle EXEEXT= BUILD_TESTS=no \
  INJECT_SRC=src/inject_linux.c WINLIST_SRC=src/winlist.c \
  MEDIA_SRC="$MEDIA_SRC" SCTP_SRC=src/sctp.c \
  CFLAGS="$BASE_CFLAGS $MEDIA_DEF -I$BSDRX_DEPS/include" \
  LDFLAGS="-L$BSDRX_DEPS/lib -Wl,-rpath,\$\$ORIGIN/../lib -Wl,--disable-new-dtags" \
  LDLIBS="$MEDIA_LIBS"

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

cat > "$APPDIR/usr/share/applications/bsdrX.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=bsdrX
Comment=Bigscreen Remote Desktop agent (cast this desktop into a VR headset)
Exec=bsdr_agent
Icon=bsdrX
Categories=Network;RemoteAccess;AudioVideo;
Terminal=true
EOF

# linuxdeploy follows ldd (honoring LD_LIBRARY_PATH) and copies non-system libs,
# skipping glibc / GPU-driver libs via its built-in exclude list. FUSE-less mode.
export APPIMAGE_EXTRACT_AND_RUN=1
LD=/opt/tools/linuxdeploy-x86_64.AppImage
"$LD" --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/bsdr_agent" \
  --desktop-file "$APPDIR/usr/share/applications/bsdrX.desktop" \
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
patchelf --set-rpath '$ORIGIN/../lib' "$PKG/opt/bsdrX/bin/bsdr_agent"
ln -sf /opt/bsdrX/bin/bsdr_agent "$PKG/usr/bin/bsdr_agent"

cp assets/../docs/../README.md "$PKG/usr/share/doc/bsdr-agent/README.md" 2>/dev/null || cp README.md "$PKG/usr/share/doc/bsdr-agent/"
cp LICENSE.md "$PKG/usr/share/doc/bsdr-agent/"
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
rm -rf "$WORK"
