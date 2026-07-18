#!/bin/sh
# fetch-onnx-gpu.sh — assemble a GPU-capable ONNX Runtime prefix for Linux/x86_64 (CUDA 12 + cuDNN 9),
# so the ONNX plugins (voice-changer RVC, 2d-3d depth, faceswap) run on an NVIDIA GPU instead of CPU.
#
# It fetches the GPU build of ONNX Runtime (which ships libonnxruntime_providers_cuda.so) and the CUDA
# 12 + cuDNN 9 RUNTIME libraries (from NVIDIA's pip wheels — no full CUDA toolkit needed), co-locates
# everything in ONE lib dir, and gives the provider libs an $ORIGIN rpath so they self-resolve. Then
# scripts/onnx-plugin-link.sh / configure auto-detect providers_cuda.so and define BSDR_ONNX_CUDA, and
# scripts/build-linux-bundle.sh bundles the runtime. The NVIDIA driver must already be installed
# (nvidia-smi works); no toolkit/nvcc is required.
#
# Usage:  scripts/fetch-onnx-gpu.sh [DEST]
#   DEST  target prefix (default: third_party/onnxruntime/linux-x64 — the same dir the CPU build uses,
#         so a plain `make plugins` / ./configure picks up the GPU EP). The CPU libonnxruntime.so is
#         backed up to lib.cpu-backup/ the first time.
#
# Requires: curl, tar, unzip, python3 -m pip (for `pip download`), patchelf. Downloads ~3 GB; the
# assembled prefix is ~3.2 GB. Idempotent-ish: re-running re-copies libs (harmless).
set -eu

ORT_VER=1.20.1
ORT_TGZ="onnxruntime-linux-x64-gpu-${ORT_VER}.tgz"
ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VER}/${ORT_TGZ}"
ORT_SHA="" # optional: set to pin the GPU tgz (sha256sum "$ORT_TGZ")

DEST="${1:-third_party/onnxruntime/linux-x64}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v patchelf >/dev/null 2>&1 || { echo "fetch-onnx-gpu: need patchelf (apt-get install patchelf)"; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "fetch-onnx-gpu: need curl"; exit 1; }

echo ">> fetching GPU ONNX Runtime $ORT_VER"
curl -fL -o "$WORK/$ORT_TGZ" "$ORT_URL"
if [ -n "$ORT_SHA" ]; then
  echo "$ORT_SHA  $WORK/$ORT_TGZ" | sha256sum -c - || { echo "fetch-onnx-gpu: SHA256 mismatch"; exit 1; }
fi
tar xzf "$WORK/$ORT_TGZ" -C "$WORK"
ORTDIR="$WORK/onnxruntime-linux-x64-gpu-${ORT_VER}"

echo ">> fetching the CUDA 12 + cuDNN 9 runtime (NVIDIA pip wheels — no toolkit)"
python3 -m pip download --no-deps -d "$WORK/wheels" \
  "nvidia-cuda-runtime-cu12" "nvidia-cublas-cu12" "nvidia-cufft-cu12" \
  "nvidia-curand-cu12" "nvidia-cudnn-cu12>=9,<10"
mkdir -p "$WORK/wheels/x"
for w in "$WORK"/wheels/*.whl; do unzip -oq "$w" -d "$WORK/wheels/x"; done

echo ">> assembling the GPU prefix at $DEST"
mkdir -p "$DEST/lib" "$DEST/include"
# back up the CPU libonnxruntime.so once
if [ ! -d "$DEST/lib.cpu-backup" ] && [ -f "$DEST/lib/libonnxruntime.so.${ORT_VER}" ]; then
  mkdir -p "$DEST/lib.cpu-backup"; cp -an "$DEST"/lib/libonnxruntime.so* "$DEST/lib.cpu-backup/" || true
fi
# GPU ORT libs (superset of the CPU one) + include (same C API + the cudnn frontend headers)
cp -f "$ORTDIR"/lib/libonnxruntime.so.${ORT_VER} \
      "$ORTDIR"/lib/libonnxruntime_providers_cuda.so \
      "$ORTDIR"/lib/libonnxruntime_providers_shared.so \
      "$ORTDIR"/lib/libonnxruntime_providers_tensorrt.so "$DEST/lib/"
ln -sf libonnxruntime.so.${ORT_VER} "$DEST/lib/libonnxruntime.so.1"
ln -sf libonnxruntime.so.1          "$DEST/lib/libonnxruntime.so"
cp -a "$ORTDIR"/include/. "$DEST/include/"
# CUDA 12 / cuDNN 9 runtime .so's (they already carry $ORIGIN rpaths, so they self-resolve here)
find "$WORK/wheels/x" -name '*.so.*' -exec cp -n {} "$DEST/lib/" \;
# the dlopened providers have no rpath -> point them at their own dir so they find the CUDA libs
patchelf --set-rpath '$ORIGIN' "$DEST/lib/libonnxruntime_providers_cuda.so"
patchelf --set-rpath '$ORIGIN' "$DEST/lib/libonnxruntime_providers_tensorrt.so"

echo ">> verifying providers_cuda.so resolves its CUDA deps"
if LD_LIBRARY_PATH="$DEST/lib" ldd "$DEST/lib/libonnxruntime_providers_cuda.so" 2>/dev/null | grep -qi 'not found'; then
  echo ">> WARN: some CUDA deps did not resolve:"; LD_LIBRARY_PATH="$DEST/lib" ldd "$DEST/lib/libonnxruntime_providers_cuda.so" | grep -i 'not found'
else
  echo ">> OK: all CUDA/cuDNN deps resolve"
fi
echo ">> done. GPU ORT prefix ready at $DEST ($(du -sh "$DEST/lib" | cut -f1)). Rebuild: make plugins"
