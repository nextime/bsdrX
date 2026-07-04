#!/usr/bin/env bash
# build-win-bundle.sh — produce a distributable Windows bundle for bsdrX:
#   * bsdr_agent.exe (media build), symbols stripped
#   * the FFmpeg DLLs it needs + the mingw runtime (libwinpthread-1.dll)
#   * README.md, LICENSE.md and an INSTALL note (Npcap / VB-CABLE / Administrator)
#   * all zipped as bsdrX-win.zip
#
# Cross-built from Linux with the MinGW-w64 toolchain against WIN_DEPS (the mingw
# dep prefix: FFmpeg DLLs + static opus/srtp2/usrsctp/openssl + the Npcap SDK).
# Runs on the host (no container). Example:
#
#   WIN_DEPS=/path/to/win-deps OUT=./dist bash scripts/build-win-bundle.sh
#
# Env: WIN_DEPS (required), OUT (default ./dist), SRC (default repo root),
#      WIN_HOST (default x86_64-w64-mingw32), VERSION (git describe or 0.0.0).
set -euo pipefail
SRC="${SRC:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${OUT:-$SRC/dist}"
WIN_HOST="${WIN_HOST:-x86_64-w64-mingw32}"
: "${WIN_DEPS:?set WIN_DEPS to the mingw dependency prefix (FFmpeg DLLs + Npcap SDK)}"
VERSION="${VERSION:-$(sed -n 's/.*BSDR_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/include/bsdr/version.h" 2>/dev/null || git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)}"
STAGE="$(mktemp -d)/bsdrX"
mkdir -p "$STAGE" "$OUT"
echo ">> bsdrX Windows bundle  version=$VERSION  deps=$WIN_DEPS"

# ---- 1. build the media .exe, then strip it ----------------------------------
cd "$SRC"
make windows-media WIN_DEPS="$WIN_DEPS" WIN_HOST="$WIN_HOST"
EXE=build-windows-media/bsdr_agent.exe
"$WIN_HOST-strip" -s "$EXE"                    # strip symbols from the agent exe only
cp "$EXE" "$STAGE/"

# ---- 2. gather the DLLs the exe imports that aren't shipped by Windows --------
# FFmpeg is linked dynamically; the mingw thread runtime too. Everything else it
# imports (kernel32, ws2_32, bcrypt, the api-ms-win-crt-* UCRT forwarders, ...) is
# part of Windows 10+. wpcap.dll is NOT bundled — it ships with the user's Npcap
# install (see the INSTALL note).
for dll in avcodec-63 avformat-63 avutil-61 avdevice-63 avfilter-12 swscale-10 swresample-7; do
  f="$WIN_DEPS/bin/$dll.dll"
  [ -f "$f" ] && cp "$f" "$STAGE/" || echo "   (warn: $dll.dll not in $WIN_DEPS/bin)"
done
WP=$(dirname "$(command -v "$WIN_HOST-gcc")")/../"$WIN_HOST"/lib/libwinpthread-1.dll
[ -f "$WP" ] || WP=/usr/"$WIN_HOST"/lib/libwinpthread-1.dll
cp "$WP" "$STAGE/"

# ---- 3. docs + a Windows-specific install note -------------------------------
cp "$SRC/README.md" "$SRC/LICENSE.md" "$STAGE/"
cat > "$STAGE/INSTALL-Windows.txt" <<'EOF'
bsdrX for Windows
=================

Run bsdr_agent.exe. It opens a control panel at http://127.0.0.1:8088 and pairs
with a Bigscreen VR headset over the LAN.

Run it AS ADMINISTRATOR — desktop capture, the virtual microphone, and the
owner-mic sniffer all need elevation.

Runtime prerequisites (install once):
  * Npcap  - https://npcap.com  (provides wpcap.dll for the owner-mic sniffer;
             install with "WinPcap API-compatible mode").
  * VB-CABLE - https://vb-audio.com/Cable/  (virtual microphone for headset audio
             and voice computer-control; the installer names the device "BSRD_Mic").

The bundled DLLs (FFmpeg av*/sw* + libwinpthread-1.dll) must stay next to
bsdr_agent.exe. Windows 10 or later is required (Universal CRT).
EOF

# ---- 4. zip ------------------------------------------------------------------
( cd "$(dirname "$STAGE")" && zip -q -r "$OUT/bsdrX-win.zip" "bsdrX" )
echo ">> bundle -> $OUT/bsdrX-win.zip"
( cd "$OUT" && ls -la bsdrX-win.zip )
rm -rf "$(dirname "$STAGE")"
