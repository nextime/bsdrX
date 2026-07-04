#!/usr/bin/env bash
# build-osx-bundle.sh — produce a distributable macOS bundle for bsdrX:
#   * a UNIVERSAL bsdr_agent (x86_64 + arm64 via lipo), symbols stripped
#   * bsdrX.app wrapping it (Info.plist + icon)
#   * bsdrX-osx.zip containing the .app, README.md and LICENSE.md
#
# Runs inside the osxcross build image (crazymax/osxcross + cross-built darwin
# openssl/opus/srtp2/usrsctp/pcap under /opt/ossl-<arch>). See the notes in
# scripts/build-linux-bundle.sh for the container pattern. Example:
#
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out bsdrx-osx-full \
#           bash /src/scripts/build-osx-bundle.sh
#
# Env: SRC (default /src), OUT (default /out), VERSION (git describe or 0.0.0).
set -euo pipefail
SRC="${SRC:-/src}"; OUT="${OUT:-/out}"; WORK="$(mktemp -d)"
VERSION="${VERSION:-$(sed -n 's/.*BSDR_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/include/bsdr/version.h" 2>/dev/null || git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)}"
echo ">> bsdrX macOS bundle  version=$VERSION"
mkdir -p "$OUT"; cp -a "$SRC" "$WORK/bsdrX"; cd "$WORK/bsdrX"
make clean >/dev/null 2>&1 || true

# ---- 1. build + strip each arch (full media), collect the two Mach-O binaries ----
declare -a BINS
for triple_pair in "o64 x86_64 x86_64-apple-darwin25.1" "oa64 arm64 arm64-apple-darwin25.1"; do
  set -- $triple_pair; host=$1; arch=$2; tr=$3
  echo ">> building macOS/$arch"
  make osxcross OSX_HOST=$host OSX_DEPS="/opt/ossl-$arch" OSX_BUILD="build-osx-$arch" BUILD_TESTS=no >/dev/null
  "$tr-strip" -Sx "build-osx-$arch/bsdr_agent"      # strip symbols from the agent only
  BINS+=("build-osx-$arch/bsdr_agent")
done

# ---- 2. lipo into one universal binary ---------------------------------------
UNI="$WORK/bsdr_agent"
lipo -create "${BINS[@]}" -output "$UNI"
echo ">> universal:"; lipo -info "$UNI"

# ---- 3. assemble bsdrX.app ---------------------------------------------------
APP="$WORK/bsdrX.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$UNI" "$APP/Contents/MacOS/bsdr_agent"
chmod +x "$APP/Contents/MacOS/bsdr_agent"
# icon: build .icns from the PNG set if png2icns is available (optional polish)
if command -v png2icns >/dev/null 2>&1; then
  png2icns "$APP/Contents/Resources/bsdrX.icns" assets/bsdrx-256.png assets/bsdrx-128.png >/dev/null 2>&1 || true
fi
ICON_LINE=""; [ -f "$APP/Contents/Resources/bsdrX.icns" ] && ICON_LINE="	<key>CFBundleIconFile</key><string>bsdrX</string>"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>CFBundleName</key><string>bsdrX</string>
	<key>CFBundleDisplayName</key><string>bsdrX</string>
	<key>CFBundleIdentifier</key><string>net.nexlab.bsdrX</string>
	<key>CFBundleVersion</key><string>${VERSION#v}</string>
	<key>CFBundleShortVersionString</key><string>${VERSION#v}</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleExecutable</key><string>bsdr_agent</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSHighResolutionCapable</key><true/>
${ICON_LINE}
</dict></plist>
EOF

# ---- 4. zip the .app + docs --------------------------------------------------
# Older bsdrx-osx-full images may predate zip being baked into the Dockerfile;
# install it on the fly so the bundle step works regardless of image vintage.
command -v zip >/dev/null 2>&1 || { apt-get update -qq && apt-get install -y -qq zip >/dev/null; }
Z="$WORK/z"; mkdir -p "$Z"
cp -a "$APP" "$Z/"
cp "$SRC/README.md" "$SRC/LICENSE.md" "$Z/"
( cd "$Z" && zip -q -y -r "$OUT/bsdrX-osx.zip" . )
echo ">> bundle -> $OUT/bsdrX-osx.zip"; ( cd "$OUT" && ls -la bsdrX-osx.zip )
rm -rf "$WORK"
