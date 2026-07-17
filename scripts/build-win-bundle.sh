#!/usr/bin/env bash
# build-win-bundle.sh — produce a distributable Windows bundle for bsdrX:
#   * bsdr_agent.exe (media build), symbols stripped
#   * the FFmpeg DLLs it needs + the mingw runtime (libwinpthread-1.dll)
#   * README.md, LICENSE.md and an INSTALL note (Npcap / VB-CABLE / Administrator)
#   * an NSIS Setup.exe (installer that also facilitates Npcap + VB-CABLE)
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

# ---- 0. optional Authenticode code-signing ----------------------------------------------------
# Windows SmartScreen warns ("unknown publisher — Run anyway?") on UNSIGNED exe/installers. Sign the
# exe AND the Setup.exe to remove/soften that. Three mutually-exclusive modes, picked in this order:
#
#   1. External signer (cloud eSigner / DigiCert KeyLocker / any CLI) — set WIN_CODESIGN_CMD to a
#      shell command that signs a file IN PLACE. "{}" in it is replaced with the file path, and the
#      path is also exported as $WIN_SIGN_FILE. Needs no osslsigncode. Examples:
#        WIN_CODESIGN_CMD='CodeSignTool sign -username=$U -password=$P -totp_secret=$T -input_file_path={}'
#        WIN_CODESIGN_CMD='smctl sign --keypair-alias KEY --input {}'
#   2. PKCS#11 hardware token / cloud HSM (SafeNet, YubiKey, Azure Key Vault) — set
#      WIN_CODESIGN_PKCS11_MODULE (the .so), plus WIN_CODESIGN_KEY (key URI/label),
#      WIN_CODESIGN_CERT (a .pem cert file) or WIN_CODESIGN_PKCS11_CERT (on-token cert id),
#      WIN_CODESIGN_PASS (PIN), and optionally WIN_CODESIGN_PKCS11_ENGINE. Needs osslsigncode.
#   3. Local .pfx/.p12 file (self-signed for testing, or a file-based cert) — set WIN_CODESIGN_PFX
#      + WIN_CODESIGN_PASS. Needs osslsigncode. A self-signed cert does NOT clear SmartScreen.
#
# WIN_CODESIGN_TS overrides the RFC-3161 timestamp URL. Unconfigured = no-op (still ships, still
# warns). An EV cert clears SmartScreen immediately; an OV/individual cert once it earns reputation.
sign_pe() {  # sign_pe <file>  — signs in place if configured; else a no-op
  local f="$1"
  local ts="${WIN_CODESIGN_TS:-http://timestamp.digicert.com}"
  local name="bsdrX" url="https://bigscreen.nexlab.net"

  # Mode 1: arbitrary external signer (cloud/CLI), signs in place.
  if [ -n "${WIN_CODESIGN_CMD:-}" ]; then
    local cmd="${WIN_CODESIGN_CMD//\{\}/$f}"
    if WIN_SIGN_FILE="$f" bash -c "$cmd" >/dev/null 2>&1; then
      echo ">> signed $(basename "$f") (external signer)"
    else
      echo "   (warn: external signer failed for $(basename "$f") — left unsigned)"
    fi
    return 0
  fi

  # Modes 2 and 3 both drive osslsigncode.
  local -a args
  if [ -n "${WIN_CODESIGN_PKCS11_MODULE:-}" ]; then          # Mode 2: PKCS#11 token / HSM
    args=(-pkcs11module "$WIN_CODESIGN_PKCS11_MODULE")
    [ -n "${WIN_CODESIGN_PKCS11_ENGINE:-}" ] && args+=(-pkcs11engine "$WIN_CODESIGN_PKCS11_ENGINE")
    [ -n "${WIN_CODESIGN_CERT:-}" ]          && args+=(-certs "$WIN_CODESIGN_CERT")
    [ -n "${WIN_CODESIGN_PKCS11_CERT:-}" ]   && args+=(-pkcs11cert "$WIN_CODESIGN_PKCS11_CERT")
    [ -n "${WIN_CODESIGN_KEY:-}" ]           && args+=(-key "$WIN_CODESIGN_KEY")
    [ -n "${WIN_CODESIGN_PASS:-}" ]          && args+=(-pass "$WIN_CODESIGN_PASS")
  elif [ -n "${WIN_CODESIGN_PFX:-}" ] && [ -f "${WIN_CODESIGN_PFX}" ]; then  # Mode 3: local .pfx
    args=(-pkcs12 "$WIN_CODESIGN_PFX" -pass "${WIN_CODESIGN_PASS:-}")
  else
    return 0  # nothing configured
  fi
  command -v osslsigncode >/dev/null 2>&1 || {
    echo "   (warn: signing configured but osslsigncode not installed — $(basename "$f") left unsigned)"; return 0; }

  if osslsigncode sign "${args[@]}" -n "$name" -i "$url" -ts "$ts" -in "$f" -out "$f.signed" >/dev/null 2>&1; then
    mv "$f.signed" "$f"; echo ">> signed $(basename "$f")"
  else
    rm -f "$f.signed"; echo "   (warn: signing failed for $(basename "$f") — left unsigned)"
  fi
}

# ---- 1. build the media .exe, then strip it ----------------------------------
cd "$SRC"
make windows-media WIN_DEPS="$WIN_DEPS" WIN_HOST="$WIN_HOST"
EXE=build-windows-media/bsdr_agent.exe
"$WIN_HOST-strip" -s "$EXE"                    # strip symbols from the agent exe only
cp "$EXE" "$STAGE/"
sign_pe "$STAGE/bsdr_agent.exe"                # sign the shipped agent (before the installer bundles it)

# ---- 2. gather the DLLs the exe imports that aren't shipped by Windows --------
# FFmpeg is linked dynamically; the mingw thread runtime too. Everything else it
# imports (kernel32, ws2_32, bcrypt, the api-ms-win-crt-* UCRT forwarders, ...) is
# part of Windows 10+. wpcap.dll is NOT bundled — it ships with the user's Npcap
# install (see the INSTALL note).
for dll in avcodec-63 avformat-63 avutil-61 avdevice-63 avfilter-12 swscale-10 swresample-7; do
  f="$WIN_DEPS/bin/$dll.dll"
  [ -f "$f" ] && cp "$f" "$STAGE/" || echo "   (warn: $dll.dll not in $WIN_DEPS/bin)"
done
# onnxruntime (in-process depth) — ship it if the deps include it (+ DirectML.dll if present)
[ -f "$WIN_DEPS/bin/onnxruntime.dll" ] && cp "$WIN_DEPS/bin/onnxruntime.dll" "$STAGE/"
[ -f "$WIN_DEPS/bin/DirectML.dll" ] && cp "$WIN_DEPS/bin/DirectML.dll" "$STAGE/"
# WinDivert (owner-mic cloud voice SUBSTITUTION): dual-licensed LGPLv3/GPLv2, so we redistribute it.
# The .dll is imported by the exe; the .sys is the kernel driver WinDivert.dll loads on first use.
if [ -f "$WIN_DEPS/bin/WinDivert.dll" ]; then
  cp "$WIN_DEPS/bin/WinDivert.dll" "$STAGE/"
  [ -f "$WIN_DEPS/bin/WinDivert64.sys" ] && cp "$WIN_DEPS/bin/WinDivert64.sys" "$STAGE/"
  [ -f "$WIN_DEPS/licenses/WinDivert-LICENSE.txt" ] && cp "$WIN_DEPS/licenses/WinDivert-LICENSE.txt" "$STAGE/"
fi
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

The agent runs WITHOUT any of the below — remote desktop, the router-companion RELAY
owner mic, and the WinDivert-based sniff fallback all work out of the box. These enable
the extra owner-mic paths and are installed once, on demand (the control panel prompts):
  * Npcap  - https://npcap.com  (wpcap.dll for the promiscuous owner-mic SNIFFER + ARP MITM;
             loaded at runtime, so it's optional — install with "WinPcap API-compatible mode").
  * VB-CABLE - https://vb-audio.com/Cable/  (virtual microphone for headset audio
             and voice computer-control; the installer names the device "BSRD_Mic").

Bundled (no separate install):
  * WinDivert (WinDivert.dll + WinDivert64.sys) - powers owner-mic cloud voice
    SUBSTITUTION (making the room hear your changed voice while you MITM the
    headset). Dual-licensed LGPLv3/GPLv2; loaded on first use, needs Administrator.
    Keep both files next to bsdr_agent.exe.

The bundled DLLs (FFmpeg av*/sw* + libwinpthread-1.dll + WinDivert) must stay next
to bsdr_agent.exe. Windows 10 or later is required (Universal CRT).
EOF

# ---- 4. NSIS installer (self-contained; facilitates Npcap + VB-CABLE) --------------------------
# Build a Setup .exe from the same staged payload and ship it INSIDE the zip alongside the portable
# folder, so users can either run the installer or unzip-and-run. No-op if makensis isn't installed.
SETUP=""
if command -v makensis >/dev/null 2>&1; then
  SETUP="$(dirname "$STAGE")/bsdrX-Setup-$VERSION.exe"
  if makensis -V2 -DVERSION="$VERSION" -DPAYLOAD="$STAGE" -DOUTFILE="$SETUP" "$SRC/scripts/bsdrx-installer.nsi"; then
    echo ">> built installer $(basename "$SETUP")"
    sign_pe "$SETUP"                            # sign the installer itself (SmartScreen looks at this)
  else
    echo "   (warn: makensis failed — shipping the portable folder only)"; SETUP=""
  fi
else
  echo "   (makensis not installed — no Setup.exe; shipping the portable folder only)"
fi

# ---- 5. zip (portable folder + the Setup.exe) --------------------------------------------------
( cd "$(dirname "$STAGE")" && zip -q -r "$OUT/bsdrX-win.zip" "bsdrX" ${SETUP:+"$(basename "$SETUP")"} )
echo ">> bundle -> $OUT/bsdrX-win.zip"
( cd "$OUT" && ls -la bsdrX-win.zip )
rm -rf "$(dirname "$STAGE")"

# ---- 6. loadable plugins (private; NOT inside the bundle) — one .dll zip per plugin --------------
# The bundle above ships NO plugin; here we cross-build each plugins/<name>/ with the same MinGW
# toolchain into a Windows .dll and package it on its own. No-op if there's no plugins/ tree.
SRC="$SRC" OUT="$OUT" VERSION="$VERSION" PLATFORM=windows ARCH=x86_64 \
  PLUGIN_CC="$WIN_HOST-gcc" PLUGIN_EXT=.dll \
  ONNX_PREFIX="$([ -f "$WIN_DEPS/include/onnxruntime_c_api.h" ] && echo "$WIN_DEPS")" \
  bash "$SRC/scripts/build-plugins.sh" || echo ">> WARN: plugin packaging failed (non-fatal)"
