#!/usr/bin/env bash
# Build a Debian package for the Linux (media-capable, dynamically-linked)
# bsdrX agent. Dependencies are computed from the binary with dpkg-shlibdeps,
# so the .deb targets the build distro's library versions (build on the oldest
# distro you want to support). For truly distro-agnostic deployment use the
# static core binary (`make linux-static`) instead.
#
#   ./packaging/build-deb.sh [VERSION]
#
# Requires: dpkg-deb, fakeroot, dpkg-shlibdeps, and the Linux media build deps.
set -euo pipefail

cd "$(dirname "$0")/.."          # repo root
VERSION="${1:-0.950.2}"
ARCH="$(dpkg --print-architecture)"
PKG="bsdrx"
ROOT="packaging/${PKG}_${VERSION}_${ARCH}"

echo "== building media-capable Linux agent =="
make >/dev/null
test -x build/bsdr_agent

echo "== staging $ROOT =="
rm -rf "$ROOT"
install -Dm755 build/bsdr_agent          "$ROOT/usr/bin/bsdr_agent"
install -Dm644 assets/bsdrx-256.png       "$ROOT/usr/share/icons/hicolor/256x256/apps/bsdrx.png"
install -Dm644 assets/bsdrx-128.png       "$ROOT/usr/share/icons/hicolor/128x128/apps/bsdrx.png"

# .desktop launcher (console agent that opens the web control panel)
install -d "$ROOT/usr/share/applications"
cat > "$ROOT/usr/share/applications/bsdrx.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=bsdrX Remote Desktop
Comment=Stream this PC to a Bigscreen headset (LAN + internet)
Exec=bsdr_agent
Icon=bsdrx
Terminal=true
Categories=Network;Utility;RemoteAccess;
Keywords=bigscreen;vr;remote;desktop;quest;
EOF

# copyright (GPLv3)
install -d "$ROOT/usr/share/doc/$PKG"
cat > "$ROOT/usr/share/doc/$PKG/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: bsdrX
Source: https://git.nexlab.net/nextime/bsrdx

Files: *
Copyright: 2026 Stefy Lanza <stefy@nexlab.net>
License: GPL-3+
 This program is free software: you can redistribute it and/or modify it under
 the terms of the GNU General Public License version 3 or (at your option) any
 later version. On Debian systems the full text is in
 /usr/share/common-licenses/GPL-3.
EOF
printf "bsdrx (%s) stable; urgency=low\n\n  * Release %s\n\n -- Stefy Lanza <stefy@nexlab.net>  %s\n" \
  "$VERSION" "$VERSION" "$(date -R 2>/dev/null || echo 'Thu, 01 Jan 1970 00:00:00 +0000')" \
  | gzip -9n > "$ROOT/usr/share/doc/$PKG/changelog.Debian.gz"

# ---- dependencies via dpkg-shlibdeps -------------------------------------
echo "== computing dependencies =="
DEPS=""
if command -v dpkg-shlibdeps >/dev/null; then
  TMP="$(mktemp -d)"
  mkdir -p "$TMP/debian"
  printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" > "$TMP/debian/control"
  : > "$TMP/debian/substvars"
  if ( cd "$TMP" && dpkg-shlibdeps -O --ignore-missing-info \
         "$OLDPWD/$ROOT/usr/bin/bsdr_agent" ) >"$TMP/out" 2>"$TMP/err"; then
    DEPS="$(sed -n 's/^shlibs:Depends=//p' "$TMP/out")"
  fi
  rm -rf "$TMP"
fi
# fallback: a sensible Debian/Ubuntu runtime set
if [ -z "$DEPS" ]; then
  echo "   dpkg-shlibdeps unavailable/failed; using a fallback dependency list"
  DEPS="ffmpeg, libssl3 | libssl3t64, libsrtp2-1, libopus0, libpulse0, libx11-6"
fi
echo "   Depends: $DEPS"

# ---- control --------------------------------------------------------------
INSTALLED_KB="$(du -sk "$ROOT/usr" | cut -f1)"
install -d "$ROOT/DEBIAN"
cat > "$ROOT/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: net
Priority: optional
Architecture: $ARCH
Depends: $DEPS
Recommends: pulseaudio
Installed-Size: $INSTALLED_KB
Maintainer: Stefy Lanza <stefy@nexlab.net>
Homepage: https://git.nexlab.net/nextime/bsrdx
Description: Bigscreen Remote Desktop agent for Linux
 Stream a Linux desktop (capture + H.264) with audio both ways and remote
 input to a Bigscreen VR headset, over the LAN or relayed via the internet.
 A clean-room reimplementation of the Bigscreen Remote Desktop PC host.
EOF

echo "== dpkg-deb --build =="
DEB="packaging/${PKG}_${VERSION}_${ARCH}.deb"
fakeroot dpkg-deb --build "$ROOT" "$DEB" >/dev/null
echo "Package: $DEB"
dpkg-deb --info "$DEB" | sed -n '/Package:/,/Description:/p'
