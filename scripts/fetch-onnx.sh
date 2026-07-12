#!/bin/sh
# fetch-onnx.sh — download the PINNED ONNX Runtime prebuilt for a platform and unpack it into a
# repo-local, git-ignored third_party/onnxruntime/<platform>/. The binary is NOT committed (it's a
# large prebuilt); this committed script + pinned version + SHA256 is what makes the build
# reproducible. Idempotent: skips a platform whose header is already present (unless --force).
#
# Usage: scripts/fetch-onnx.sh [--force] [platform ...]
#   platform: linux-x64 | osx-arm64 | win-x64   (default: auto-detect the host)
#
# ONNX Runtime is a huge C++/CMake project; we do NOT build it from source — we download Microsoft's
# official release binary and verify its SHA256. To bump the version, change ORT_VER and refresh the
# three hashes (sha256sum onnxruntime-<plat>-<ver>.{tgz,zip}).
set -eu

ORT_VER=1.20.1
BASE="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VER}"

# asset + sha256 per platform
asset_for() {
    case "$1" in
        linux-x64) echo "onnxruntime-linux-x64-${ORT_VER}.tgz" ;;
        osx-arm64) echo "onnxruntime-osx-arm64-${ORT_VER}.tgz" ;;
        win-x64)   echo "onnxruntime-win-x64-${ORT_VER}.zip" ;;
        *) return 1 ;;
    esac
}
sha_for() {
    case "$1" in
        linux-x64) echo "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105" ;;
        osx-arm64) echo "b678fc3c2354c771fea4fba420edeccfba205140088334df801e7fc40e83a57a" ;;
        win-x64)   echo "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581" ;;
        *) return 1 ;;
    esac
}

host_platform() {
    _s=$(uname -s 2>/dev/null || echo unknown); _m=$(uname -m 2>/dev/null || echo unknown)
    case "$_s" in
        Linux)  echo "linux-x64" ;;
        Darwin) echo "osx-arm64" ;;
        MINGW*|MSYS*|CYGWIN*) echo "win-x64" ;;
        *) echo "linux-x64" ;;
    esac
}

sha256_of() {  # portable sha256 -> stdout
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else echo "fetch-onnx: no sha256sum/shasum available" >&2; return 1; fi
}

download() {  # url dest
    if command -v curl >/dev/null 2>&1; then curl -fSL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then wget -O "$2" "$1"
    else echo "fetch-onnx: need curl or wget" >&2; return 1; fi
}

# repo root = parent of this script's dir
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
dest_root="$root/third_party/onnxruntime"

force=0; platforms=""
for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) platforms="$platforms $arg" ;;
    esac
done
[ -n "$platforms" ] || platforms=$(host_platform)

rc=0
for plat in $platforms; do
    asset=$(asset_for "$plat") || { echo "fetch-onnx: unknown platform '$plat'" >&2; rc=1; continue; }
    sha=$(sha_for "$plat")
    dest="$dest_root/$plat"
    if [ "$force" -eq 0 ] && [ -f "$dest/include/onnxruntime_c_api.h" ]; then
        echo "fetch-onnx: $plat already present at $dest (use --force to refresh)"
        continue
    fi
    tmp=$(mktemp -d)
    echo "fetch-onnx: downloading $asset (ONNX Runtime $ORT_VER, $plat) ..."
    if ! download "$BASE/$asset" "$tmp/$asset"; then
        echo "fetch-onnx: download failed for $plat" >&2; rm -rf "$tmp"; rc=1; continue
    fi
    got=$(sha256_of "$tmp/$asset")
    if [ "$got" != "$sha" ]; then
        echo "fetch-onnx: SHA256 MISMATCH for $asset" >&2
        echo "  expected $sha" >&2; echo "  got      $got" >&2
        rm -rf "$tmp"; rc=1; continue
    fi
    echo "fetch-onnx: sha256 OK, extracting -> $dest"
    rm -rf "$dest"; mkdir -p "$dest"
    case "$asset" in
        *.tgz|*.tar.gz) tar -xzf "$tmp/$asset" -C "$dest" --strip-components=1 ;;
        *.zip)
            unzip -q "$tmp/$asset" -d "$tmp/ex"
            inner=$(find "$tmp/ex" -maxdepth 1 -mindepth 1 -type d | head -1)
            cp -a "$inner"/. "$dest"/ ;;
    esac
    rm -rf "$tmp"
    if [ -f "$dest/include/onnxruntime_c_api.h" ]; then
        echo "fetch-onnx: $plat ready ($dest)"
    else
        echo "fetch-onnx: extraction did not yield include/onnxruntime_c_api.h for $plat" >&2; rc=1
    fi
done
exit $rc
