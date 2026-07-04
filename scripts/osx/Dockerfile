# syntax=docker/dockerfile:1
#
# Runnable macOS cross-compile environment for bsdrX, layered on top of the
# crazy-max osxcross *toolchain carrier* image (default tag: osxcross:local,
# produced by `docker buildx bake` in /storage/osxcross/docker-osxcross).
#
# The carrier image is FROM scratch (just /osxcross + /osxsdk), so it can't run a
# shell — this wraps it in Ubuntu with clang/lld/make/cmake so we can drive
# `make osx` and the OpenSSL cross-build inside a container.
#
#   docker build -t bsdrx-osxcross -f scripts/osx/Dockerfile \
#     --build-arg OSXCROSS_IMAGE=osxcross:local scripts/osx
#
# (scripts/build-osx.sh does this for you.)
ARG OSXCROSS_IMAGE=osxcross:local
FROM ${OSXCROSS_IMAGE} AS osxcross

FROM ubuntu:24.04
RUN export DEBIAN_FRONTEND=noninteractive \
  && apt-get update \
  && apt-get install --no-install-recommends -y \
       bash clang lld llvm libc6-dev make cmake perl xz-utils \
       ca-certificates curl git file \
  && rm -rf /var/lib/apt/lists/*

# Bring in the osxcross toolchain (the macOS SDK is bundled under /osxcross/SDK,
# so the o64-/oa64- wrappers already know their sysroot).
COPY --from=osxcross /osxcross /osxcross
ENV PATH="/osxcross/bin:${PATH}"
ENV LD_LIBRARY_PATH="/osxcross/lib:${LD_LIBRARY_PATH}"
ENV OSXCROSS_ROOT="/osxcross"

WORKDIR /src
