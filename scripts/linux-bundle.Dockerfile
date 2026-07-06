# Build environment for scripts/build-linux-bundle.sh.
#
# Ubuntu 20.04 (glibc 2.31) base so the produced binaries run on 20.04+ /
# Debian 11+ / RHEL 8+. Builds a PRIVATE, MINIMAL ffmpeg (+ openssl3, x264, opus,
# libsrtp2, usrsctp, libpcap) into /opt/bsdrx-deps — only the codecs/muxers/devices
# bsdrX actually uses (H.264 via nvenc/x264, mjpeg screenshots, x11grab capture,
# file demux + h264 bitstream filter), so the dependency graph stays tiny instead
# of the ~200-lib tail a distro ffmpeg drags in. Also installs linuxdeploy +
# appimagetool + dpkg-dev + zip so the bundle script can package in-place.
#
#   docker build -f scripts/linux-bundle.Dockerfile -t bsdrx-linux-deps scripts
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out bsdrx-linux-deps \
#           bash /src/scripts/build-linux-bundle.sh
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential yasm nasm pkg-config git curl ca-certificates \
      autoconf automake libtool cmake dpkg-dev fakeroot zip flex bison \
      libx11-dev libxext-dev libxfixes-dev libxcb1-dev libxcb-shm0-dev \
      libxcb-shape0-dev libxcb-xfixes0-dev libpulse-dev libdrm-dev \
      libpipewire-0.3-dev libdbus-1-dev \
      python3 file patchelf \
    && rm -rf /var/lib/apt/lists/*

ENV PFX=/opt/bsdrx-deps
ENV PKG_CONFIG_PATH=$PFX/lib/pkgconfig
ENV LD_LIBRARY_PATH=$PFX/lib
ENV PATH=$PFX/bin:$PATH

# ---- OpenSSL 3 (20.04 ships 1.1) ----
RUN set -eux; cd /tmp; \
    curl -fsSL https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz | tar xz; \
    cd openssl-3.0.15; \
    ./Configure linux-x86_64 shared --prefix=$PFX --libdir=lib --openssldir=$PFX/etc/ssl; \
    make -j"$(nproc)" >/tmp/ossl.log 2>&1 && make install_sw >/dev/null 2>&1; \
    rm -rf /tmp/openssl-3.0.15

# ---- x264 ----
RUN set -eux; cd /tmp; \
    git clone --depth 1 https://code.videolan.org/videolan/x264.git; \
    cd x264 && ./configure --prefix=$PFX --enable-shared --disable-cli --bit-depth=8 \
      && make -j"$(nproc)" >/tmp/x264.log 2>&1 && make install >/dev/null 2>&1; \
    rm -rf /tmp/x264

# ---- opus / libsrtp2 / usrsctp / libpcap ----
RUN set -eux; cd /tmp; \
    curl -fsSL https://github.com/xiph/opus/releases/download/v1.4/opus-1.4.tar.gz | tar xz; \
    cd opus-1.4 && ./configure --prefix=$PFX --enable-shared --disable-static --disable-doc --disable-extra-programs \
      >/tmp/opus.log 2>&1 && make -j"$(nproc)" >>/tmp/opus.log 2>&1 && make install >/dev/null 2>&1; cd /tmp; rm -rf opus-1.4; \
    curl -fsSL https://github.com/cisco/libsrtp/archive/refs/tags/v2.5.0.tar.gz | tar xz; \
    cd libsrtp-2.5.0 && cmake -S . -B bld -DCMAKE_INSTALL_PREFIX=$PFX -DBUILD_SHARED_LIBS=ON -DTEST_APPS=OFF \
      >/tmp/srtp.log 2>&1 && cmake --build bld -j"$(nproc)" >>/tmp/srtp.log 2>&1 && cmake --install bld >/dev/null 2>&1; cd /tmp; rm -rf libsrtp-2.5.0; \
    curl -fsSL https://github.com/sctplab/usrsctp/archive/refs/tags/0.9.5.0.tar.gz | tar xz; \
    cd usrsctp-0.9.5.0 && cmake -S . -B bld -DCMAKE_INSTALL_PREFIX=$PFX -Dsctp_build_shared_lib=ON \
      -Dsctp_build_programs=OFF -Dsctp_build_fuzzer=OFF -Dsctp_werror=OFF \
      >/tmp/usrsctp.log 2>&1 && cmake --build bld -j"$(nproc)" >>/tmp/usrsctp.log 2>&1 && cmake --install bld >/dev/null 2>&1; cd /tmp; rm -rf usrsctp-0.9.5.0; \
    curl -fsSL https://github.com/the-tcpdump-group/libpcap/archive/refs/tags/libpcap-1.10.4.tar.gz | tar xz; \
    cd libpcap-libpcap-1.10.4 && ./configure --prefix=$PFX --enable-shared --disable-static --without-libnl --disable-dbus --disable-rdma \
      >/tmp/pcap.log 2>&1 && make -j"$(nproc)" >>/tmp/pcap.log 2>&1 && make install >/dev/null 2>&1; cd /tmp; rm -rf libpcap-libpcap-1.10.4

# ---- nv-codec-headers (NVENC: dlopens the driver at runtime, no link dep) ----
RUN set -eux; cd /tmp; \
    git clone --depth 1 --branch n12.2.72.0 https://github.com/FFmpeg/nv-codec-headers.git; \
    cd nv-codec-headers && make install PREFIX=$PFX >/dev/null 2>&1; rm -rf /tmp/nv-codec-headers

# ---- MINIMAL ffmpeg: only what bsdrX uses ----
RUN set -eux; export PKG_CONFIG_PATH="$PFX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"; cd /tmp; \
    curl -fsSL https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz | tar xJ; \
    cd ffmpeg-6.1.2; \
    ./configure --prefix=$PFX --enable-shared --disable-static \
      --disable-everything --disable-doc --disable-programs --disable-network \
      --enable-gpl --enable-version3 \
      --enable-libx264 --enable-nvenc \
      --enable-encoder=libx264,h264_nvenc,mjpeg,rawvideo \
      --enable-decoder=h264,mjpeg,rawvideo,pcm_s16le \
      --enable-parser=h264,mjpeg \
      --enable-demuxer=h264,mov,matroska,mpegts,mjpeg,rawvideo,image2 \
      --enable-muxer=rawvideo,mjpeg,mp4,h264,image2 \
      --enable-protocol=file,pipe \
      --enable-libxcb --enable-indev=x11grab \
      --enable-filter=scale,format,null,copy,hflip,vflip,transpose \
      --enable-bsf=h264_mp4toannexb,extract_extradata \
      --enable-swscale --enable-swresample --enable-avdevice \
      --extra-cflags="-I$PFX/include" --extra-ldflags="-L$PFX/lib"; \
    make -j"$(nproc)" >/tmp/ffmpeg.log 2>&1 && make install >/dev/null 2>&1; \
    cd /tmp; rm -rf ffmpeg-6.1.2

# ---- ONNX Runtime (in-process depth; CPU/XNNPACK). Shipped in the AppImage/.deb lib dir. ----
RUN set -eux; cd /tmp; ORT_VER=1.20.1; \
    curl -fsSL -o ort.tgz "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VER/onnxruntime-linux-x64-$ORT_VER.tgz"; \
    tar xzf ort.tgz; D="onnxruntime-linux-x64-$ORT_VER"; \
    cp -a "$D"/include/* "$PFX/include/"; \
    cp -a "$D"/lib/libonnxruntime.so* "$PFX/lib/"; \
    rm -rf ort.tgz "$D"

# ---- AppImage tooling (FUSE-less: run with APPIMAGE_EXTRACT_AND_RUN=1) ----
RUN set -eux; mkdir -p /opt/tools; cd /opt/tools; \
    curl -fsSL -o linuxdeploy-x86_64.AppImage \
      https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage; \
    curl -fsSL -o appimagetool-x86_64.AppImage \
      https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage; \
    chmod +x /opt/tools/*.AppImage
ENV PATH=/opt/tools:$PATH

# ---- packaging helpers appimagetool looks for (kept in a LATE layer so adding them
#      doesn't invalidate the expensive dep-build layers above): appstreamcli validates
#      the shipped AppStream metainfo, gpg is the (optional) AppImage signer. Their
#      absence is what triggered the "appstreamcli/gpg is missing" warnings. ----
RUN apt-get update && apt-get install -y --no-install-recommends appstream gnupg \
    && rm -rf /var/lib/apt/lists/*
