#!/usr/bin/env bash
# Cross-build the macOS dependency set (OpenSSL, static) for bsdrX, per arch,
# into osx-deps/<arch>. Meant to run INSIDE the bsdrx-osxcross container (the
# osxcross o64-/oa64- wrappers must be on PATH) — scripts/build-osx.sh drives it.
# Mirrors scripts/build-win-deps.sh / build-android-deps.sh.
#
#   ./scripts/build-osx-deps.sh [x86_64|arm64 ...]      (default: both)
#
# bsdrX's macOS build is core-only (input channel + discovery + control + DTLS
# transport + LAN replay); the media stack (ScreenCaptureKit/CoreAudio shims) is
# a separate later phase, so OpenSSL is the only dependency here.
set -euo pipefail

OPENSSL_VER=3.0.15
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/osx-deps/src"
ARCHES=("$@"); [ ${#ARCHES[@]} -gt 0 ] || ARCHES=(x86_64 arm64)

wrapper()     { case "$1" in x86_64) echo o64;; arm64) echo oa64;;
  *) echo "unknown arch $1" >&2; exit 1;; esac; }
ossl_target() { case "$1" in x86_64) echo darwin64-x86_64-cc;; arm64) echo darwin64-arm64-cc;; esac; }

command -v o64-clang >/dev/null 2>&1 || {
  echo "osxcross not on PATH (run inside bsdrx-osxcross; see scripts/build-osx.sh)"; exit 1; }

mkdir -p "$SRC"; cd "$SRC"
[ -d "openssl-$OPENSSL_VER" ] || {
  curl -fsSL -o openssl.tar.gz \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
  tar xzf openssl.tar.gz; }

for A in "${ARCHES[@]}"; do
  PREFIX="$ROOT/osx-deps/$A"; W="$(wrapper "$A")"
  if [ -f "$PREFIX/lib/libssl.a" ]; then echo "== OpenSSL ($A) present =="; continue; fi
  echo "== OpenSSL $OPENSSL_VER ($A, $W-clang) =="
  ( cd "openssl-$OPENSSL_VER"
    make clean >/dev/null 2>&1 || true
    CC="$W-clang" AR="$W-ar" RANLIB="$W-ranlib" \
      ./Configure "$(ossl_target "$A")" no-shared no-tests no-asm --prefix="$PREFIX"
    make -j"$(nproc)" CC="$W-clang" AR="$W-ar" RANLIB="$W-ranlib" build_libs
    make install_dev
    make clean >/dev/null 2>&1 || true )
  # OpenSSL may install to lib64 on some targets; mirror into lib/
  [ -d "$PREFIX/lib64" ] && cp -n "$PREFIX"/lib64/*.a "$PREFIX/lib/" 2>/dev/null || true
  echo "  $A: $(cd "$PREFIX/lib" && echo *.a)"
done

echo
echo "macOS deps ready under: $ROOT/osx-deps/<arch>"
