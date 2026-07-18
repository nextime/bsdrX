#!/bin/sh
# pack-gpu-cuda-plugin.sh — package the GPU CUDA provider as a bsdrX "payload" store plugin.
#
# The result is a normal plugin zip the store hosts and the agent downloads (with the progress bar), but
# instead of a loadable <slug>.so it carries a "bsdr-payload.conf" (target=onnx-provider) + the ONNX
# Runtime CUDA provider libs. The agent's store installer (src/plugstore.c: install_payload) recognizes
# the manifest and drops the provider .so's beside libonnxruntime.so, so the ONNX plugins (voice/2d-3d/
# faceswap) can use CUDA — no ~700 MB base bundle. The CUDA 12 / cuDNN 9 runtime is HOST-provided; absent
# it, ORT can't load the provider and select_ep() falls back to CPU (surfaced honestly in the web UI).
#
# Usage:  scripts/pack-gpu-cuda-plugin.sh [ORT_PREFIX] [OUT_ZIP]
#   ORT_PREFIX  GPU ORT prefix with lib/libonnxruntime_providers_cuda.so (default third_party/onnxruntime/linux-x64;
#               populate it first with scripts/fetch-onnx-gpu.sh)
#   OUT_ZIP     output zip (default dist/gpu-cuda-linux-x86_64.zip)
#
# Then publish it to the store as slug "gpu-cuda" (platform linux, arch x86_64) with scripts/push_plugin.py
# or the store admin UI. Recommended store copy: mark it PUBLIC/free, and RECOMMEND it for voice-changer /
# 2d-3d / faceswap (a soft hint — NOT a hard dependency, since those must still run on CPU without it).
set -eu

ORT="${1:-third_party/onnxruntime/linux-x64}"
OUT="${2:-dist/gpu-cuda-linux-x86_64.zip}"
PROV="$ORT/lib/libonnxruntime_providers_cuda.so"
SHARED="$ORT/lib/libonnxruntime_providers_shared.so"

[ -f "$PROV" ]   || { echo "pack-gpu-cuda: no $PROV (run scripts/fetch-onnx-gpu.sh first)"; exit 1; }
[ -f "$SHARED" ] || { echo "pack-gpu-cuda: no $SHARED"; exit 1; }
command -v zip >/dev/null 2>&1 || { echo "pack-gpu-cuda: need zip"; exit 1; }

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cp "$PROV" "$SHARED" "$W/"
# The provider has no rpath of its own; $ORIGIN lets it find CUDA libs installed beside it (if a host
# ships them there) before falling through to the system CUDA on the loader path.
if command -v patchelf >/dev/null 2>&1; then
  patchelf --set-rpath '$ORIGIN' "$W/libonnxruntime_providers_cuda.so" 2>/dev/null || true
fi
printf 'target=onnx-provider\n' > "$W/bsdr-payload.conf"

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
( cd "$W" && zip -q -X -r "$OLDPWD/$OUT" . )
echo "packaged $OUT ($(du -h "$OUT" | cut -f1)) — contents:"
( cd "$W" && ls -la ) | sed 's/^/    /'
echo "publish it to the store as slug 'gpu-cuda' (platform=linux arch=x86_64), PUBLIC/free."
