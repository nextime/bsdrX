# Build environment for scripts/build-osx-bundle.sh — the macOS (universal) bundle.
#
# Builds on the osxcross carrier image (osxcross:local, produced by
# `docker buildx bake` in /storage/osxcross/docker-osxcross) and cross-builds the
# darwin STATIC dependency set — openssl + opus + libsrtp2 + usrsctp + libpcap —
# for BOTH architectures into /opt/ossl-x86_64 and /opt/ossl-arm64. bsdrX's macOS
# build links these statically and uses the CoreAudio/AudioToolbox frameworks, so
# there is no bundled ffmpeg (hence the small .app).
#
#   docker build -f scripts/osx-bundle.Dockerfile -t bsdrx-osx-full scripts
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out bsdrx-osx-full \
#           bash /src/scripts/build-osx-bundle.sh
FROM osxcross:local
ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
      curl ca-certificates git cmake make pkg-config file xz-utils zip icnsutils \
    && rm -rf /var/lib/apt/lists/*

# osxcross wrappers must be on PATH (o64-clang / oa64-clang etc.). The carrier
# image usually exports this already; set a sensible default if not.
ENV PATH=/usr/osxcross/bin:${PATH}
ENV MACOSX_DEPLOYMENT_TARGET=11.0

# Cross-build every dep for both arches. o64 = x86_64, oa64 = arm64; prefixes are
# /opt/ossl-x86_64 and /opt/ossl-arm64 (matching build-osx-bundle.sh's OSX_DEPS).
RUN set -euxo pipefail; cd /tmp; \
    build_arch() { \
      local arch="$1" W ossl host; \
      case "$arch" in x86_64) W=o64;  ossl=darwin64-x86_64-cc; host=x86_64-apple-darwin  ;; \
                      arm64)  W=oa64; ossl=darwin64-arm64-cc;  host=aarch64-apple-darwin ;; esac; \
      local PFX=/opt/ossl-$arch; mkdir -p "$PFX"; \
      export CC="$W-clang" CXX="$W-clang++" AR="$W-ar" RANLIB="$W-ranlib" \
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
          -DCMAKE_C_COMPILER="$W-clang" -DCMAKE_AR="/usr/osxcross/bin/$W-ar" \
          -DCMAKE_RANLIB="/usr/osxcross/bin/$W-ranlib" -DCMAKE_INSTALL_PREFIX="$PFX" \
          -DBUILD_SHARED_LIBS=OFF -DTEST_APPS=OFF >/dev/null; \
        cmake --build b -j"$(nproc)" >/dev/null && cmake --install b >/dev/null ); \
      echo "==== usrsctp ($arch) ===="; ( cd usrsctp-0.9.5.0 && rm -rf b && \
        cmake -S . -B b -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$arch" \
          -DCMAKE_C_COMPILER="$W-clang" -DCMAKE_AR="/usr/osxcross/bin/$W-ar" \
          -DCMAKE_RANLIB="/usr/osxcross/bin/$W-ranlib" -DCMAKE_INSTALL_PREFIX="$PFX" \
          -Dsctp_build_shared_lib=OFF -Dsctp_build_programs=OFF -Dsctp_build_fuzzer=OFF \
          -Dsctp_werror=OFF >/dev/null; \
        cmake --build b -j"$(nproc)" >/dev/null && cmake --install b >/dev/null ); \
      echo "==== libpcap ($arch) ===="; ( cd libpcap-libpcap-1.10.4 && make clean >/dev/null 2>&1 || true; \
        CFLAGS="-Wno-implicit-function-declaration -Wno-error" \
        ./configure --host="$host" --prefix="$PFX" --disable-shared --enable-static \
          --without-libnl --disable-dbus --disable-rdma >/dev/null; \
        make -j"$(nproc)" >/dev/null && make install >/dev/null ); \
      echo "==== $arch deps: $(cd "$PFX/lib" && echo *.a) ===="; \
    }; \
    curl -fsSL https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz | tar xz; \
    curl -fsSL https://github.com/xiph/opus/releases/download/v1.4/opus-1.4.tar.gz | tar xz; \
    curl -fsSL https://github.com/cisco/libsrtp/archive/refs/tags/v2.5.0.tar.gz | tar xz; \
    curl -fsSL https://github.com/sctplab/usrsctp/archive/refs/tags/0.9.5.0.tar.gz | tar xz; \
    curl -fsSL https://github.com/the-tcpdump-group/libpcap/archive/refs/tags/libpcap-1.10.4.tar.gz | tar xz; \
    build_arch x86_64; \
    build_arch arm64; \
    rm -rf /tmp/openssl-3.0.15 /tmp/opus-1.4 /tmp/libsrtp-2.5.0 /tmp/usrsctp-0.9.5.0 /tmp/libpcap-libpcap-1.10.4
