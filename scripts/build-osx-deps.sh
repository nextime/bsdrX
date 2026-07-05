#!/usr/bin/env bash
# Cross-build the FULL macOS dependency set for the media-capable bsdrX agent,
# one static prefix per arch under osx-deps/<arch>. Meant to run INSIDE the
# bsdrx-osxcross container (the osxcross o64-/oa64- wrappers must be on PATH) —
# scripts/build-osx.sh / build-osx-bundle.sh drive it. Mirrors
# scripts/build-android-deps.sh / build-win-deps.sh.
#
#   ./scripts/build-osx-deps.sh [x86_64|arm64 ...]      (default: both)
#
# Produces, per arch:
#   OpenSSL 3.x   (libssl.a/libcrypto.a)   - TLS/DTLS/SRTP keying
#   libopus       (libopus.a)              - audio codec
#   libsrtp2      (libsrtp2.a)             - SRTP
#   usrsctp       (libusrsctp.a)           - DataChannel
#   libpcap       (libpcap.a)              - owner-mic sniffer (BPF on macOS)
#   ffmpeg        (libav*/libsw*.a)        - desktop capture + encode + file demux
#                 configured --enable-videotoolbox --enable-avfoundation so
#                 capture.c can grab the screen (avfoundation) and H.264-encode
#                 (h264_videotoolbox); full default codec set stays on for
#                 filesrc.c local-file streaming.
#
# NOTE: the ffmpeg videotoolbox/avfoundation cross-build needs the macOS SDK
# frameworks that osxcross provides; it is the least-exercised part of this
# script and may need flag tweaks for your osxcross SDK version. Validate a run
# in the bsdrx-osxcross image before relying on it.
set -euo pipefail

OPENSSL_VER=3.0.15
OPUS_VER=1.5.2
SRTP_VER=2.6.0
PCAP_VER=1.10.5
FFMPEG_VER=7.1
ORT_VER=1.20.1

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/osx-deps/src"
ARCHES=("$@"); [ ${#ARCHES[@]} -gt 0 ] || ARCHES=(x86_64 arm64)

wrapper()     { case "$1" in x86_64) echo o64;; arm64) echo oa64;;
  *) echo "unknown arch $1" >&2; exit 1;; esac; }
ossl_target() { case "$1" in x86_64) echo darwin64-x86_64-cc;; arm64) echo darwin64-arm64-cc;; esac; }
ff_arch()     { case "$1" in x86_64) echo x86_64;; arm64) echo aarch64;; esac; }

command -v o64-clang >/dev/null 2>&1 || {
  echo "osxcross not on PATH (run inside bsdrx-osxcross; see scripts/build-osx.sh)"; exit 1; }

mkdir -p "$SRC"; cd "$SRC"
# --- fetch sources -----------------------------------------------------------
[ -d "openssl-$OPENSSL_VER" ] || { curl -fsSL -o o.tgz \
  "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"; tar xzf o.tgz; }
[ -d "opus-$OPUS_VER" ] || { curl -fsSL -o opus.tgz \
  "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"; tar xzf opus.tgz; }
[ -d "libsrtp-$SRTP_VER" ] || { curl -fsSL -o srtp.tgz \
  "https://github.com/cisco/libsrtp/archive/refs/tags/v$SRTP_VER.tar.gz"; tar xzf srtp.tgz; }
[ -d "usrsctp-master" ] || { curl -fsSL -o sctp.tgz \
  "https://github.com/sctplab/usrsctp/archive/refs/heads/master.tar.gz"; tar xzf sctp.tgz; }
[ -d "libpcap-$PCAP_VER" ] || { curl -fsSL -o pcap.tgz \
  "https://www.tcpdump.org/release/libpcap-$PCAP_VER.tar.gz"; tar xzf pcap.tgz; }
[ -d "ffmpeg-$FFMPEG_VER" ] || { curl -fsSL -o ff.txz \
  "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"; tar xf ff.txz; }

for A in "${ARCHES[@]}"; do
  PREFIX="$ROOT/osx-deps/$A"; W="$(wrapper "$A")"
  # ar/ranlib exist only under the full darwin triple (e.g. x86_64-apple-darwin25.1-ar);
  # osxcross has NO o64-ar. Find them by arch prefix in the toolchain bin dir — robust to
  # the SDK's darwin version (and to -dumpmachine's debug chatter).
  BINDIR="$(dirname "$(command -v "$W-clang")")"
  case "$A" in x86_64) ARPFX=x86_64-apple-darwin ;; arm64) ARPFX=arm64-apple-darwin ;; esac
  AR="$(ls "$BINDIR"/${ARPFX}*-ar 2>/dev/null | head -1)"
  RANLIB="$(ls "$BINDIR"/${ARPFX}*-ranlib 2>/dev/null | head -1)"
  [ -n "$AR" ] && [ -n "$RANLIB" ] || { echo "no darwin ar/ranlib for $A under $BINDIR" >&2; exit 1; }
  mkdir -p "$PREFIX/lib" "$PREFIX/include"
  echo "==== $A -> $PREFIX  ($W-clang, ar=$(basename "$AR")) ===="

  cmake_dep() { # dir extra-args...
    local dir="$1"; shift
    rm -rf "$dir/build-$A"
    cmake -B "$dir/build-$A" -S "$dir" -G "Unix Makefiles" \
      -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$A" \
      -DCMAKE_C_COMPILER="$W-clang" -DCMAKE_CXX_COMPILER="$W-clang++" \
      -DCMAKE_AR="$(command -v "$AR")" -DCMAKE_RANLIB="$(command -v "$RANLIB")" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF "$@"
    cmake --build "$dir/build-$A" -j"$(nproc)" --target install
  }

  echo "== OpenSSL $OPENSSL_VER =="
  if [ ! -f "$PREFIX/lib/libssl.a" ]; then
    ( cd "openssl-$OPENSSL_VER"
      make clean >/dev/null 2>&1 || true
      CC="$W-clang" AR="$AR" RANLIB="$RANLIB" \
        ./Configure "$(ossl_target "$A")" no-shared no-tests no-asm --prefix="$PREFIX"
      make -j"$(nproc)" CC="$W-clang" AR="$AR" RANLIB="$RANLIB" build_libs
      make install_dev
      make clean >/dev/null 2>&1 || true )
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

  echo "== libpcap $PCAP_VER =="
  [ -f "$PREFIX/lib/libpcap.a" ] || \
    cmake_dep "libpcap-$PCAP_VER" -DDISABLE_DBUS=ON -DDISABLE_RDMA=ON -DBUILD_WITH_LIBNL=OFF

  echo "== ffmpeg $FFMPEG_VER (videotoolbox + avfoundation) =="
  if [ ! -f "$PREFIX/lib/libavcodec.a" ]; then
    ( cd "ffmpeg-$FFMPEG_VER"
      make distclean >/dev/null 2>&1 || true
      ./configure \
        --prefix="$PREFIX" \
        --enable-cross-compile --target-os=darwin --arch="$(ff_arch "$A")" \
        --cc="$W-clang" --cxx="$W-clang++" --ar="$AR" --ranlib="$RANLIB" \
        --enable-static --disable-shared --enable-pic --disable-asm \
        --disable-everything --disable-programs --disable-doc --disable-debug --disable-network \
        --enable-videotoolbox --enable-avfoundation \
        --enable-encoder=h264_videotoolbox,mjpeg,rawvideo \
        --enable-decoder=h264,hevc,aac,mp3,ac3,mjpeg,rawvideo,pcm_s16le,pcm_f32le \
        --enable-parser=h264,hevc,aac,mjpeg \
        --enable-demuxer=h264,hevc,mov,matroska,mpegts,mjpeg,rawvideo,image2,aac,mp3,wav \
        --enable-muxer=rawvideo,mjpeg,mp4,h264,image2 \
        --enable-protocol=file,pipe --enable-indev=avfoundation \
        --enable-filter=scale,format,null,copy,hflip,vflip,transpose \
        --enable-bsf=h264_mp4toannexb,extract_extradata \
        --enable-swscale --enable-swresample --enable-avdevice \
        --pkg-config=pkg-config --pkg-config-flags=--static
      make -j"$(nproc)"
      make install )
  fi

  echo "== onnxruntime $ORT_VER (in-process depth; CoreML EP built in) =="
  if [ ! -f "$PREFIX/lib/libonnxruntime.dylib" ]; then
    oa="$([ "$A" = arm64 ] && echo arm64 || echo x86_64)"
    [ -d "onnxruntime-osx-$oa-$ORT_VER" ] || { curl -fsSL -o ort.tgz \
      "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VER/onnxruntime-osx-$oa-$ORT_VER.tgz"; tar xzf ort.tgz; rm -f ort.tgz; }
    cp -a "onnxruntime-osx-$oa-$ORT_VER"/include/* "$PREFIX/include/"
    cp -a "onnxruntime-osx-$oa-$ORT_VER"/lib/libonnxruntime*.dylib "$PREFIX/lib/"
  fi
  echo "  $A deps: $(cd "$PREFIX/lib" && echo *.a) + onnxruntime.dylib"
done

echo
echo "macOS deps ready under: $ROOT/osx-deps/<arch>"
