#!/usr/bin/env bash
# Cross-build the Android (NDK) dependency set for the media-capable bsdrX agent,
# one static prefix per ABI under android/deps/<abi>. The Gradle/CMake build
# points at these. Mirrors scripts/build-win-deps.sh.
#
#   ./scripts/build-android-deps.sh [ABI ...]        (default: arm64-v8a)
#
# Produces, per ABI (e.g. arm64-v8a, x86_64):
#   OpenSSL 3.x   (static libssl.a/libcrypto.a)   - TLS/DTLS/SRTP keying
#   libopus       (static libopus.a)              - audio codec
#   libsrtp2      (static libsrtp2.a)             - SRTP
#   usrsctp       (static libusrsctp.a)           - DataChannel
# (No ffmpeg/X11/PulseAudio: capture is MediaCodec, audio is AAudio — both in Kotlin.)
#
# Requires: Android NDK (ANDROID_NDK_HOME / ANDROID_NDK_ROOT, or auto-detected
# under $ANDROID_HOME/ndk/*), cmake, curl, tar.
set -euo pipefail

API=29                           # minSdk: AudioPlaybackCapture (system audio)
OPENSSL_VER=3.0.15
OPUS_VER=1.5.2
SRTP_VER=2.6.0
ORT_VER=1.20.0

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/android/deps/src"
ABIS=("$@"); [ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a)

# locate the NDK
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ]; then
  SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/android-sdk}}"
  NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "Android NDK not found; set ANDROID_NDK_HOME"; exit 1; }
echo "NDK: $NDK"

HOSTTAG="linux-x86_64"
export PATH="$NDK/toolchains/llvm/prebuilt/$HOSTTAG/bin:$PATH"

openssl_target() { case "$1" in
  arm64-v8a)   echo android-arm64;;
  x86_64)      echo android-x86_64;;
  armeabi-v7a) echo android-arm;;
  x86)         echo android-x86;;
  *) echo "unknown ABI $1" >&2; exit 1;; esac; }

mkdir -p "$SRC"; cd "$SRC"
[ -d "openssl-$OPENSSL_VER" ] || { curl -fsSL -o o.tgz \
  "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"; tar xzf o.tgz; }
[ -d "opus-$OPUS_VER" ] || { curl -fsSL -o opus.tgz \
  "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"; tar xzf opus.tgz; }
[ -d "libsrtp-$SRTP_VER" ] || { curl -fsSL -o srtp.tgz \
  "https://github.com/cisco/libsrtp/archive/refs/tags/v$SRTP_VER.tar.gz"; tar xzf srtp.tgz; }
[ -d "usrsctp-master" ] || { curl -fsSL -o sctp.tgz \
  "https://github.com/sctplab/usrsctp/archive/refs/heads/master.tar.gz"; tar xzf sctp.tgz; }

for ABI in "${ABIS[@]}"; do
  PREFIX="$ROOT/android/deps/$ABI"
  mkdir -p "$PREFIX/lib" "$PREFIX/include"
  echo "==== $ABI -> $PREFIX ===="

  cmake_dep() { # dir extra-args...
    local dir="$1"; shift
    rm -rf "$dir/build-$ABI"
    cmake -B "$dir/build-$ABI" -S "$dir" -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF "$@"
    cmake --build "$dir/build-$ABI" -j"$(nproc)" --target install
  }

  echo "== OpenSSL $OPENSSL_VER =="
  if [ ! -f "$PREFIX/lib/libssl.a" ]; then
    ( cd "openssl-$OPENSSL_VER"
      export ANDROID_NDK_ROOT="$NDK"
      ./Configure "$(openssl_target "$ABI")" "-D__ANDROID_API__=$API" no-shared no-tests \
        --prefix="$PREFIX" --openssldir="$PREFIX/ssl"
      make -j"$(nproc)" build_libs
      make install_dev
      make clean )
    [ -d "$PREFIX/lib64" ] && cp -n "$PREFIX"/lib64/*.a "$PREFIX/lib/" 2>/dev/null || true
  fi

  echo "== libopus $OPUS_VER =="
  [ -f "$PREFIX/lib/libopus.a" ] || \
    cmake_dep "opus-$OPUS_VER" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF

  echo "== libsrtp2 $SRTP_VER =="
  [ -f "$PREFIX/lib/libsrtp2.a" ] || \
    cmake_dep "libsrtp-$SRTP_VER" -DTEST_APPS=OFF -DLIBSRTP_TEST_APPS=OFF -DENABLE_WARNINGS_AS_ERRORS=OFF

  echo "== usrsctp =="
  [ -f "$PREFIX/lib/libusrsctp.a" ] || \
    cmake_dep "usrsctp-master" -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF

  echo "== onnxruntime $ORT_VER (in-process depth; NNAPI EP) =="
  if [ ! -f "$PREFIX/lib/libonnxruntime.so" ]; then
    [ -f "onnxruntime-android-$ORT_VER.aar" ] || curl -fsSL -o "onnxruntime-android-$ORT_VER.aar" \
      "https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/$ORT_VER/onnxruntime-android-$ORT_VER.aar"
    rm -rf aar-$ABI && mkdir aar-$ABI && ( cd aar-$ABI && unzip -q "../onnxruntime-android-$ORT_VER.aar" )
    cp -a aar-$ABI/headers/* "$PREFIX/include/"
    cp -a "aar-$ABI/jni/$ABI/libonnxruntime.so" "$PREFIX/lib/"
    # also stage the runtime .so so Gradle packages it into the APK
    mkdir -p "$ROOT/android/app/src/main/jniLibs/$ABI"
    cp -a "aar-$ABI/jni/$ABI/libonnxruntime.so" "$ROOT/android/app/src/main/jniLibs/$ABI/"
    rm -rf aar-$ABI
  fi

  echo "  $ABI deps: $(cd "$PREFIX/lib" && echo *.a) + libonnxruntime.so"
done

echo
echo "Android deps ready under: $ROOT/android/deps/<abi>"
