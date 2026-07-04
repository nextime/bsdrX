#!/usr/bin/env bash
# Cross-compile bsdrX for macOS from Linux using the crazy-max osxcross Docker
# toolchain. Builds OpenSSL + the core agent for x86_64 and arm64, then lipo's a
# universal Mach-O binary.
#
#   ./scripts/build-osx.sh [x86_64|arm64|universal]     (default: universal)
#
# Requires Docker + the osxcross carrier image (default osxcross:local, built via
# `docker buildx bake` in /storage/osxcross/docker-osxcross). Override with:
#   OSXCROSS_IMAGE=ghcr.io/crazy-max/osxcross:latest ./scripts/build-osx.sh
# If your Docker needs sudo, run:  DOCKER='sudo docker' ./scripts/build-osx.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OSXCROSS_IMAGE="${OSXCROSS_IMAGE:-osxcross:local}"
BUILDER_IMAGE="${BUILDER_IMAGE:-bsdrx-osxcross}"
DOCKER="${DOCKER:-docker}"
TARGET="${1:-universal}"

# 1. ensure the carrier image exists
if ! $DOCKER image inspect "$OSXCROSS_IMAGE" >/dev/null 2>&1; then
  echo "osxcross carrier image '$OSXCROSS_IMAGE' not found."
  echo "Build it first:  (cd /storage/osxcross/docker-osxcross && docker buildx bake)"
  echo "or set OSXCROSS_IMAGE to a published tag (e.g. ghcr.io/crazy-max/osxcross:latest)."
  exit 1
fi

# 2. ensure the runnable cross env exists (rebuild if the carrier is newer)
if ! $DOCKER image inspect "$BUILDER_IMAGE" >/dev/null 2>&1; then
  echo "== building $BUILDER_IMAGE from $OSXCROSS_IMAGE =="
  $DOCKER build -t "$BUILDER_IMAGE" -f "$ROOT/scripts/osx/Dockerfile" \
    --build-arg "OSXCROSS_IMAGE=$OSXCROSS_IMAGE" "$ROOT/scripts/osx"
fi

run() { $DOCKER run --rm -v "$ROOT:/src" -w /src "$BUILDER_IMAGE" bash -lc "$1"; }

build_arch() { # arch wrapper
  local A="$1" W="$2"
  echo "== macOS deps + agent: $A ($W) =="
  run "scripts/build-osx-deps.sh $A && \
       make osx OSX_HOST=$W OSX_OPENSSL=/src/osx-deps/$A OSX_BUILD=build-osx-$A"
}

case "$TARGET" in
  x86_64)    build_arch x86_64 o64 ;;
  arm64)     build_arch arm64  oa64 ;;
  universal)
    build_arch x86_64 o64
    build_arch arm64  oa64
    echo "== lipo universal -> build-osx/bsdr_agent =="
    run "mkdir -p build-osx && \
         o64-lipo -create -output build-osx/bsdr_agent \
           build-osx-x86_64/bsdr_agent build-osx-arm64/bsdr_agent && \
         file build-osx/bsdr_agent"
    ;;
  *) echo "usage: $0 [x86_64|arm64|universal]"; exit 1 ;;
esac

echo "macOS build complete."
