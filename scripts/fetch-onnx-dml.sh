#!/bin/sh
# fetch-onnx-dml.sh — install a DirectML-enabled ONNX Runtime into the Windows deps prefix, so the ONNX
# plugins (voice-changer RVC, 2d-3d depth, faceswap) run on ANY Windows GPU (NVIDIA/AMD/Intel) via
# DirectML — no CUDA needed. It fetches the DirectML build of onnxruntime.dll + dml_provider_factory.h
# (Microsoft.ML.OnnxRuntime.DirectML) and DirectML.dll (Microsoft.AI.DirectML), and drops them into
# WIN_DEPS. Then scripts/onnx-plugin-link.sh auto-detects dml_provider_factory.h and defines
# BSDR_ONNX_DML, and scripts/build-win-bundle.sh ships onnxruntime.dll + DirectML.dll beside the exe.
# No GPU -> select_ep() falls back to CPU (surfaced honestly in the web UI).
#
# Usage:  WIN_DEPS=/path/to/win-deps scripts/fetch-onnx-dml.sh
# Requires: curl, unzip. Backs up the CPU onnxruntime.dll to onnxruntime.dll.cpu-backup the first time.
set -eu

ORT_VER="${ORT_VER:-1.20.1}"
DML_VER="${DML_VER:-1.15.4}"
: "${WIN_DEPS:?set WIN_DEPS to the mingw dependency prefix}"
NUGET="https://api.nuget.org/v3-flatcontainer"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

echo ">> fetching Microsoft.ML.OnnxRuntime.DirectML $ORT_VER"
curl -fsSL -o "$W/ort-dml.nupkg" "$NUGET/microsoft.ml.onnxruntime.directml/$ORT_VER/microsoft.ml.onnxruntime.directml.$ORT_VER.nupkg"
echo ">> fetching Microsoft.AI.DirectML $DML_VER"
curl -fsSL -o "$W/directml.nupkg" "$NUGET/microsoft.ai.directml/$DML_VER/microsoft.ai.directml.$DML_VER.nupkg"

mkdir -p "$W/x"
( cd "$W/x" && unzip -oq ../ort-dml.nupkg && unzip -oq ../directml.nupkg )

mkdir -p "$WIN_DEPS/bin" "$WIN_DEPS/include"
[ -f "$WIN_DEPS/bin/onnxruntime.dll" ] && [ ! -f "$WIN_DEPS/bin/onnxruntime.dll.cpu-backup" ] && \
  cp -an "$WIN_DEPS/bin/onnxruntime.dll" "$WIN_DEPS/bin/onnxruntime.dll.cpu-backup" || true
cp -f "$W/x/runtimes/win-x64/native/onnxruntime.dll" "$WIN_DEPS/bin/onnxruntime.dll"
cp -f "$W/x/bin/x64-win/DirectML.dll"                 "$WIN_DEPS/bin/DirectML.dll"
cp -f "$W/x/build/native/include/dml_provider_factory.h" "$WIN_DEPS/include/dml_provider_factory.h"

echo ">> installed DirectML ONNX Runtime into $WIN_DEPS:"
echo "     bin/onnxruntime.dll (DirectML), bin/DirectML.dll, include/dml_provider_factory.h"
echo ">> the plugin build now auto-defines BSDR_ONNX_DML; rebuild: make windows-media WIN_DEPS=$WIN_DEPS"
