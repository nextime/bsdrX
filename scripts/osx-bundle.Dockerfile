# Build environment for scripts/build-osx-bundle.sh — the macOS (universal) bundle.
#
# Builds on the osxcross carrier image (osxcross:local, produced by
# `docker buildx bake` in /storage/osxcross/docker-osxcross) and cross-builds the
# darwin STATIC dependency set — openssl + opus + libsrtp2 + usrsctp + libpcap +
# ffmpeg (videotoolbox/avfoundation) — for BOTH architectures into /opt/ossl-x86_64
# and /opt/ossl-arm64. bsdrX's macOS build links these statically and uses the
# CoreAudio/AudioToolbox/VideoToolbox frameworks. ffmpeg drives desktop capture
# (avfoundation grab + h264_videotoolbox encode) and local-file streaming; it is
# the same dep set as scripts/build-osx-deps.sh (kept in sync manually).
#
#   docker build -f scripts/osx-bundle.Dockerfile -t bsdrx-osx-full scripts
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out bsdrx-osx-full \
#           bash /src/scripts/build-osx-bundle.sh
# osxcross:local (from the tpoechtrager/osxcross bake) is a SCRATCH image holding
# ONLY the toolchain under /osxcross — no shell, no apt. So consume it as a COPY
# source on top of a real Ubuntu base (the pattern from docker-osxcross's README),
# NOT as the FROM base directly (which fails: no /bin/bash to run any RUN).
FROM osxcross:local AS osxcross

# ubuntu:24.04 — the osxcross toolchain binaries are built against glibc 2.38 /
# GLIBCXX 3.4.32, so an older base (22.04 = glibc 2.35) can't run o64-clang.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

# Build tools + what the osxcross clang wrappers need on the host side (clang/lld/
# libc6-dev), plus gcc for ffmpeg's HOST-cc probe (it builds build-time tools with
# gcc, distinct from the darwin target cc), nasm for ffmpeg's x86_64 asm, and
# xz/zip/icnsutils for packaging.
RUN apt-get update && apt-get install -y --no-install-recommends \
      curl ca-certificates git cmake make pkg-config file xz-utils zip icnsutils \
      clang lld libc6-dev gcc nasm flex bison \
    && rm -rf /var/lib/apt/lists/*

# Bring in the darwin cross-toolchain + macOS SDK from the carrier image.
COPY --from=osxcross /osxcross /osxcross
ENV PATH=/osxcross/bin:${PATH}
ENV LD_LIBRARY_PATH=/osxcross/lib
ENV MACOSX_DEPLOYMENT_TARGET=11.0

# Cross-build every dep for both arches. o64 = x86_64, oa64 = arm64; prefixes are
# /opt/ossl-x86_64 and /opt/ossl-arm64 (matching build-osx-bundle.sh's OSX_DEPS).
RUN set -euxo pipefail; cd /tmp; \
    build_arch() { \
      local arch="$1" W ossl host artriple; \
      case "$arch" in x86_64) W=o64;  ossl=darwin64-x86_64-cc; host=x86_64-apple-darwin;  artriple=x86_64-apple-darwin ;; \
                      arm64)  W=oa64; ossl=darwin64-arm64-cc;  host=aarch64-apple-darwin; artriple=arm64-apple-darwin ;; esac; \
      local PFX=/opt/ossl-$arch; mkdir -p "$PFX"; \
      local AR RANLIB; \
      AR="$(ls /osxcross/bin/${artriple}*-ar 2>/dev/null | head -1)"; \
      RANLIB="$(ls /osxcross/bin/${artriple}*-ranlib 2>/dev/null | head -1)"; \
      [ -n "$AR" ] && [ -n "$RANLIB" ] || { echo "no darwin ar/ranlib for $arch in /osxcross/bin" >&2; exit 1; }; \
      export CC="$W-clang" CXX="$W-clang++" AR RANLIB \
             PKG_CONFIG_PATH="$PFX/lib/pkgconfig" ; \
      echo "==== OpenSSL ($arch) ===="; ( cd openssl-3.0.15 && make clean >/dev/null 2>&1 || true; \
        ./Configure "$ossl" no-shared no-tests no-asm --prefix="$PFX"; \
        make -j"$(nproc)" build_libs && make install_dev && make clean >/dev/null 2>&1 || true ); \
      [ -d "$PFX/lib64" ] && cp -n "$PFX"/lib64/*.a "$PFX/lib/" 2>/dev/null || true; \
      echo "==== opus ($arch) ===="; ( cd opus-1.4 && make clean >/dev/null 2>&1 || true; \
        ./configure --host="$host" --prefix="$PFX" --disable-shared --enable-static \
          --disable-rtcd --disable-intrinsics --disable-doc --disable-extra-programs >/dev/null; \
        make -j"$(nproc)" >/dev/null && make install >/dev/null ); \
      echo "==== libsrtp2 ($arch) ===="; ( cd libsrtp-2.5.0 && rm -rf b && \
        cmake -S . -B b -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$arch" \
          -DCMAKE_C_COMPILER="$W-clang" -DCMAKE_AR="$AR" \
          -DCMAKE_RANLIB="$RANLIB" -DCMAKE_INSTALL_PREFIX="$PFX" \
          -DBUILD_SHARED_LIBS=OFF -DTEST_APPS=OFF >/dev/null; \
        cmake --build b -j"$(nproc)" >/dev/null && cmake --install b >/dev/null ); \
      echo "==== usrsctp ($arch) ===="; ( cd usrsctp-0.9.5.0 && rm -rf b && \
        cmake -S . -B b -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$arch" \
          -DCMAKE_C_COMPILER="$W-clang" -DCMAKE_AR="$AR" \
          -DCMAKE_RANLIB="$RANLIB" -DCMAKE_INSTALL_PREFIX="$PFX" \
          -Dsctp_build_shared_lib=OFF -Dsctp_build_programs=OFF -Dsctp_build_fuzzer=OFF \
          -Dsctp_werror=OFF >/dev/null; \
        cmake --build b -j"$(nproc)" >/dev/null && cmake --install b >/dev/null ); \
      echo "==== libpcap ($arch) ===="; ( cd libpcap-libpcap-1.10.4 && make clean >/dev/null 2>&1 || true; \
        CFLAGS="-Wno-implicit-function-declaration -Wno-error" \
        ./configure --host="$host" --prefix="$PFX" --disable-shared --enable-static \
          --without-libnl --disable-dbus --disable-rdma >/dev/null; \
        make -j"$(nproc)" >/dev/null && make install >/dev/null ); \
      echo "==== ffmpeg ($arch) ===="; local ffarch; \
      case "$arch" in x86_64) ffarch=x86_64 ;; arm64) ffarch=aarch64 ;; esac; \
      ( cd ffmpeg-7.1 && make distclean >/dev/null 2>&1 || true; \
        ./configure --prefix="$PFX" \
          --enable-cross-compile --target-os=darwin --arch="$ffarch" \
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
          --pkg-config=pkg-config --pkg-config-flags=--static >/dev/null; \
        make -j"$(nproc)" >/dev/null && make install >/dev/null ); \
      echo "==== onnxruntime ($arch) ===="; local ortdir; \
      case "$arch" in x86_64) ortdir=onnxruntime-osx-x86_64-1.20.1 ;; arm64) ortdir=onnxruntime-osx-arm64-1.20.1 ;; esac; \
      cp -a "$ortdir"/include/* "$PFX/include/"; \
      cp -a "$ortdir"/lib/libonnxruntime*.dylib "$PFX/lib/"; \
      echo "==== $arch deps: $(cd "$PFX/lib" && echo *.a) + onnxruntime.dylib ===="; \
    }; \
    curl -fsSL https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz | tar xz; \
    curl -fsSL https://github.com/xiph/opus/releases/download/v1.4/opus-1.4.tar.gz | tar xz; \
    curl -fsSL https://github.com/cisco/libsrtp/archive/refs/tags/v2.5.0.tar.gz | tar xz; \
    curl -fsSL https://github.com/sctplab/usrsctp/archive/refs/tags/0.9.5.0.tar.gz | tar xz; \
    curl -fsSL https://github.com/the-tcpdump-group/libpcap/archive/refs/tags/libpcap-1.10.4.tar.gz | tar xz; \
    curl -fsSL https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz | tar xJ; \
    curl -fsSL https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-x86_64-1.20.1.tgz | tar xz; \
    curl -fsSL https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-arm64-1.20.1.tgz | tar xz; \
    build_arch x86_64; \
    build_arch arm64; \
    rm -rf /tmp/openssl-3.0.15 /tmp/opus-1.4 /tmp/libsrtp-2.5.0 /tmp/usrsctp-0.9.5.0 /tmp/libpcap-libpcap-1.10.4 /tmp/ffmpeg-7.1 \
           /tmp/onnxruntime-osx-x86_64-1.20.1 /tmp/onnxruntime-osx-arm64-1.20.1
