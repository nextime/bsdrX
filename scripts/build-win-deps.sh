#!/usr/bin/env bash
# Cross-build the Windows (mingw-w64) dependency set for the media-capable
# bsdrX agent, into a single prefix. Run on Linux with mingw-w64 + cmake.
#
#   ./scripts/build-win-deps.sh [PREFIX]      (default: ./win-deps)
#
# Produces, for x86_64-w64-mingw32:
#   OpenSSL 3.x   (static libssl.a/libcrypto.a)     - TLS/DTLS/SRTP keying
#   libopus       (static libopus.a)                - audio codec
#   libsrtp2      (static libsrtp2.a)               - SRTP
#   usrsctp       (static libusrsctp.a)             - DataChannel
#   FFmpeg        (BtbN prebuilt: .dll.a import libs + headers + bundled DLLs)
#                                                   - capture (gdigrab) + H.264
# The FFmpeg DLLs are staged under $PREFIX/bin for the installer to bundle.
set -euo pipefail

HOST=x86_64-w64-mingw32
PREFIX="${1:-$(pwd)/win-deps}"
SRC="$(pwd)/win-src"
JOBS="$(nproc)"
TOOLCHAIN="$(cd "$(dirname "$0")" && pwd)/mingw-toolchain.cmake"

OPENSSL_VER=3.0.15
OPUS_VER=1.5.2
SRTP_VER=2.6.0
FFMPEG_URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$PREFIX/bin" "$SRC"
cd "$SRC"

cmake_dep() { # dir extra-cmake-args...
  local dir="$1"; shift
  cmake -B "$dir/build-mingw" -G "Unix Makefiles" -S "$dir" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DWIN_DEPS_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-D__USE_MINGW_ANSI_STDIO=1" "$@"
  cmake --build "$dir/build-mingw" -j"$JOBS" --target install
}

echo "== OpenSSL $OPENSSL_VER =="
[ -f "$PREFIX/lib/libssl.a" ] || {
  curl -fsSL -o openssl.tar.gz \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
  tar xzf openssl.tar.gz; cd "openssl-$OPENSSL_VER"
  ./Configure mingw64 no-shared no-tests --cross-compile-prefix="$HOST-" --prefix="$PREFIX"
  make -j"$JOBS" build_libs && make install_dev
  # OpenSSL installs to lib64 on this target; mirror into lib/
  [ -d "$PREFIX/lib64" ] && cp -n "$PREFIX"/lib64/*.a "$PREFIX/lib/" || true
  cd "$SRC"
}

echo "== libopus $OPUS_VER =="
[ -f "$PREFIX/lib/libopus.a" ] || {
  curl -fsSL -o opus.tar.gz "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
  tar xzf opus.tar.gz
  cmake_dep "opus-$OPUS_VER" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF
}

echo "== libsrtp2 $SRTP_VER =="
[ -f "$PREFIX/lib/libsrtp2.a" ] || {
  curl -fsSL -o libsrtp.tar.gz \
    "https://github.com/cisco/libsrtp/archive/refs/tags/v$SRTP_VER.tar.gz"
  tar xzf libsrtp.tar.gz
  cmake_dep "libsrtp-$SRTP_VER" -DTEST_APPS=OFF -DLIBSRTP_TEST_APPS=OFF \
            -DENABLE_WARNINGS_AS_ERRORS=OFF
}

echo "== usrsctp =="
[ -f "$PREFIX/lib/libusrsctp.a" ] || {
  curl -fsSL -o usrsctp.tar.gz \
    "https://github.com/sctplab/usrsctp/archive/refs/heads/master.tar.gz"
  tar xzf usrsctp.tar.gz
  cmake_dep "usrsctp-master" -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF
}

echo "== FFmpeg (prebuilt mingw, BtbN) =="
[ -f "$PREFIX/lib/libavcodec.dll.a" ] || {
  curl -fsSL -o ffmpeg-win64.zip "$FFMPEG_URL"
  rm -rf ffmpeg-dl && mkdir ffmpeg-dl && unzip -q -o ffmpeg-win64.zip -d ffmpeg-dl
  D="$(echo ffmpeg-dl/*/)"
  cp -r "$D"/include/* "$PREFIX/include/"
  for def in "$D"/lib/*.def; do
    base="$(basename "$def" | sed -E 's/-[0-9]+\.def$//')"
    dll="$(basename "$def" .def).dll"
    "$HOST-dlltool" -d "$def" -D "$dll" -l "$PREFIX/lib/lib${base}.dll.a"
  done
  cp "$D"/bin/*.dll "$PREFIX/bin/"
}

echo
echo "Windows deps ready in: $PREFIX"
echo "  static: $(cd "$PREFIX/lib" && echo *.a)"
echo "  ffmpeg: $(cd "$PREFIX/lib" && echo *.dll.a)"
echo "  bundle DLLs: $PREFIX/bin/*.dll"
