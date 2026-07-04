#!/usr/bin/env bash
# Stage the media-capable Windows agent + FFmpeg DLLs + assets (+ optional
# VB-CABLE) and build the NSIS installer. Run on Linux with mingw-w64 + makensis.
#
#   WIN_DEPS=../win-deps ./installer/build-installer.sh [VERSION]
#
# Requires: win-deps built (scripts/build-win-deps.sh), makensis in PATH.
# Optional: drop VBCABLE_Setup_x64.exe (+ its files) into installer/vendor/vbcable
#           to bundle the virtual-mic driver; otherwise that step is skipped and
#           the installer only renames a pre-installed VB-CABLE to BSRD_Mic.
set -euo pipefail

cd "$(dirname "$0")/.."          # repo root
VERSION="${1:-0.950.2}"
WIN_DEPS="${WIN_DEPS:-$(pwd)/../win-deps}"
STAGE="installer/stage"

[ -d "$WIN_DEPS/bin" ] || { echo "WIN_DEPS=$WIN_DEPS has no bin/ (run scripts/build-win-deps.sh)"; exit 1; }

echo "== building media-capable agent =="
make windows-media WIN_DEPS="$WIN_DEPS"

echo "== staging =="
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp build-windows-media/bsdr_agent.exe "$STAGE/"
cp assets/bsdrx.ico                   "$STAGE/"
cp installer/rename-mic.ps1           "$STAGE/"
cp "$WIN_DEPS"/bin/*.dll              "$STAGE/"     # FFmpeg runtime

NSIS_DEFS=(-DVERSION="$VERSION")
if [ -f installer/vendor/vbcable/VBCABLE_Setup_x64.exe ]; then
  echo "   bundling VB-CABLE"
  mkdir -p "$STAGE/vbcable"
  cp -r installer/vendor/vbcable/* "$STAGE/vbcable/"
  NSIS_DEFS+=(-DVBCABLE=1)
else
  echo "   (no installer/vendor/vbcable/VBCABLE_Setup_x64.exe — installer will only rename a pre-installed VB-CABLE)"
fi

echo "== makensis =="
( cd installer && makensis "${NSIS_DEFS[@]}" bsdrx.nsi )
echo
echo "Installer ready: installer/bsdrX-Setup-${VERSION}.exe"
