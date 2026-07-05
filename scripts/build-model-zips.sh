#!/usr/bin/env bash
# build-model-zips.sh — fetch the depth-estimation models bsdrX's in-process 2D->3D tiers use and
# package them into the distributable zips users can import by hand (Web UI "Import model zip" or
# --threed-model-import). Produces, in OUT (default dist/):
#     bsdrX-model-cpu.zip    (tier 1: Depth-Anything-V2-Small, Apache-2.0)
#     bsdrX-model-gpu.zip    (tier 2: MiDaS DPT-Hybrid, MIT)
#     bsdrX-model-hi.zip     (tier 3: MiDaS DPT-BEiT-Large, MIT)
#     bsdrX-models.zip       (all three)
# Each zip contains the canonically-named .onnx + a manifest.json. Every model is GPLv3-
# redistributable (Apache/MIT); Depth-Anything-V2 Base/Large are CC-BY-NC and are intentionally NOT
# shipped. Prints each SHA-256 so you can pin it in src/model_store.c's catalog.
#
# A tier whose URL is empty below is skipped with a note (pin the URL when finalized).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/dist}"
WORK="$(mktemp -d)"
mkdir -p "$OUT"

# tier | canonical name | filename | preprocess | input | url   (keep in sync with model_store.c)
MODELS=(
  "1|depth-anything-v2-small|depth-anything-v2-small.onnx|dav2|518|https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx"
  "2|midas-dpt-hybrid|midas-dpt-hybrid-384.onnx|midas|384|https://huggingface.co/Xenova/dpt-hybrid-midas/resolve/main/onnx/model.onnx"
  "3|midas-dpt-large|midas-dpt-large-384.onnx|midas|384|https://huggingface.co/Xenova/dpt-large/resolve/main/onnx/model.onnx"
)
tier_zip() { case "$1" in 1) echo cpu;; 2) echo gpu;; 3) echo hi;; esac; }

declare -a HAVE
for m in "${MODELS[@]}"; do
  IFS='|' read -r tier name file pp insz url <<<"$m"
  if [ -z "$url" ]; then echo ">> tier $tier ($name): no URL pinned yet — skipping"; continue; fi
  echo ">> tier $tier: fetching $name"
  curl -fsSL -o "$WORK/$file" "$url"
  sha="$(sha256sum "$WORK/$file" | awk '{print $1}')"
  printf '[{"tier":%s,"name":"%s","file":"%s","preprocess":"%s","input_size":%s,"sha256":"%s"}]\n' \
    "$tier" "$name" "$file" "$pp" "$insz" "$sha" > "$WORK/$name.manifest.json"
  cp "$WORK/$name.manifest.json" "$WORK/manifest.json"
  ( cd "$WORK" && rm -f "$OUT/bsdrX-model-$(tier_zip "$tier").zip" \
    && zip -q -j "$OUT/bsdrX-model-$(tier_zip "$tier").zip" "$file" manifest.json )
  echo "   $name  sha256=$sha  -> bsdrX-model-$(tier_zip "$tier").zip"
  HAVE+=("$file")
done

# combined all-three zip (one manifest listing every present model)
if [ ${#HAVE[@]} -gt 0 ]; then
  {
    echo -n '['
    first=1
    for m in "${MODELS[@]}"; do
      IFS='|' read -r tier name file pp insz url <<<"$m"
      [ -f "$WORK/$file" ] || continue
      sha="$(sha256sum "$WORK/$file" | awk '{print $1}')"
      [ $first -eq 1 ] || echo -n ','
      printf '{"tier":%s,"name":"%s","file":"%s","preprocess":"%s","input_size":%s,"sha256":"%s"}' \
        "$tier" "$name" "$file" "$pp" "$insz" "$sha"
      first=0
    done
    echo ']'
  } > "$WORK/manifest.json"
  ( cd "$WORK" && rm -f "$OUT/bsdrX-models.zip" && zip -q -j "$OUT/bsdrX-models.zip" "${HAVE[@]}" manifest.json )
  echo ">> combined: bsdrX-models.zip (${#HAVE[@]} model(s))"
fi

rm -rf "$WORK"
echo "model zips ready in: $OUT"
