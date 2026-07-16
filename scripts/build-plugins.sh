#!/usr/bin/env bash
# build-plugins.sh — build every loadable plugin (plugins/<name>/) for ONE platform+arch using the
# caller's toolchain, and package EACH plugin on its own as:
#
#     $OUT/bsdrX-plugin-<name>-<PLATFORM>-<ARCH>-<VERSION>.zip
#
# One zip per plugin per platform (never combined, never inside the app bundle). A plugin links
# nothing but libc + include/bsdr/plugin.h, so a single-compiler invocation cross-builds it — the
# caller just supplies its own CC/EXT/flags (Windows mingw, macOS osxcross, Android NDK, Linux native).
#
# Called by each platform's bundle step (build-{linux,win,osx}-bundle.sh, distribute.sh build_android).
# Builds whatever plugins/<name>/ are present and works fine when there are none: an empty or absent
# plugins/ tree => exits 0 quietly. (A public github clone carries only the 'open' plugins, if any;
# it still builds them, and simply skips the store-upload step below since it lacks push_plugin.py.)
#
# OPTIONAL store upload: if the private uploader scripts/push_plugin.py is present AND both
# PLUGSTORE_URL and PLUGSTORE_TOKEN are set, each packaged zip is pushed to the bsdrX plugin store
# right after it's built. The uploader is stripped from the public GitHub mirror (see
# publish-github.sh), so a github clone simply never has it and this step is skipped — the packaging
# above is unaffected. Per-plugin store metadata (slug/visibility/price/…) comes from an optional
# plugins/<name>/store.conf (key=value; see that file for keys); missing => sensible private defaults.
#
# Env:
#   SRC          repo root (default: parent of this script)
#   OUT          output dir for the zips (default: $SRC/dist)
#   VERSION      version string (default: parsed from include/bsdr/version.h)
#   PLATFORM     platform label for the zip name, e.g. linux|windows|macos|android   (required)
#   ARCH         arch label for the zip name, e.g. x86_64|aarch64|arm64|arm64-v8a    (required)
#   PLUGIN_CC    the C compiler to use                                               (required)
#   PLUGIN_EXT   shared-object extension: .so (Linux/macOS/Android) or .dll (Windows) (default .so)
#   PLUGIN_CFLAGS  extra compiler flags (e.g. -target … --sysroot … for NDK/osxcross) (optional)
#   PLUGSTORE_URL    base URL of the plugin store, e.g. https://plugins.nexlab.net   (enables upload)
#   PLUGSTORE_TOKEN  upload API token (bstk_…)                                        (enables upload)
#   PLUGSTORE_PY     path to the uploader (default: this script's dir/push_plugin.py)
#   PYTHON           python interpreter for the uploader (default: python3)
set -euo pipefail
SRC="${SRC:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${OUT:-$SRC/dist}"
VERSION="${VERSION:-$(sed -n 's/.*BSDR_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/include/bsdr/version.h" 2>/dev/null || echo 0.0.0)}"
# Plugin ABI these builds target — parsed from the plugin header (the exact define, not _MIN).
# Goes into the zip name and the store so the agent only downloads a build its ABI can load.
PLUGIN_ABI="$(sed -n 's/^#define[[:space:]]\+BSDR_PLUGIN_ABI[[:space:]]\+\([0-9]\+\).*/\1/p' "$SRC/include/bsdr/plugin.h" 2>/dev/null | head -1)"
PLUGIN_ABI="${PLUGIN_ABI:-0}"
: "${PLATFORM:?set PLATFORM (linux|windows|macos|android)}"
: "${ARCH:?set ARCH (x86_64|aarch64|arm64|arm64-v8a|…)}"
: "${PLUGIN_CC:?set PLUGIN_CC to the target C compiler}"
PLUGIN_EXT="${PLUGIN_EXT:-.so}"
PLUGIN_CFLAGS="${PLUGIN_CFLAGS:-}"

# --- optional plugin-store upload -------------------------------------------------------------
# Opt-in only: uploads happen when PUSH_PLUGINS=1 (distribute.sh's --push-plugins sets this) AND the
# private uploader + credentials are present. A public github clone lacks push_plugin.py, so PUSH
# stays 0 there and nothing below runs — packaging is unaffected. Credentials come from the environment
# or a private, gitignored $SRC/.plugstore.env (PLUGSTORE_URL / PLUGSTORE_TOKEN).
PLUGSTORE_PY="${PLUGSTORE_PY:-$(dirname "$0")/push_plugin.py}"
PYTHON="${PYTHON:-python3}"
if [ -f "$SRC/.plugstore.env" ]; then
  # shellcheck disable=SC1091
  . "$SRC/.plugstore.env"
fi
PUSH=0
if [ "${PUSH_PLUGINS:-0}" = 1 ]; then
  if [ -z "${PLUGSTORE_URL:-}" ] || [ -z "${PLUGSTORE_TOKEN:-}" ]; then
    echo ">> plugins: --push-plugins set but no PLUGSTORE_URL/PLUGSTORE_TOKEN — upload skipped" >&2
  elif [ ! -f "$PLUGSTORE_PY" ]; then
    echo ">> plugins: --push-plugins set but uploader not present ($PLUGSTORE_PY) — upload skipped" >&2
  elif ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo ">> plugins: --push-plugins set but '$PYTHON' not found — upload skipped" >&2
  else
    PUSH=1
    echo ">> plugins: store upload ENABLED -> $PLUGSTORE_URL"
  fi
fi

# store_push <plugin-dir> <plugin-name> <zip> <plugin-version> — push one packaged zip to the store,
# using per-plugin metadata from <plugin-dir>/store.conf. MARKING (private|closed|open) sets the
# store's initial state; the store configures/inherits price. DESCRIPTION is required. The plugin's
# OWN version and PLUGIN_ABI are sent so the store can serve the newest ABI-compatible build. Never
# fatal: a failed upload only warns.
store_push() {
  local d="$1" name="$2" zip="$3" pver="$4"
  # defaults (safe: a plugin with no store.conf uploads as PRIVATE, never public)
  local SLUG="$name" DISPLAY_NAME="$name" MARKING="private" \
        SUMMARY="" DESCRIPTION="" EXTRA="" ABI_MAX="0" DEPENDS=""
  if [ -f "$d/store.conf" ]; then
    # shellcheck disable=SC1090
    . "$d/store.conf"
  fi
  case "$MARKING" in
    private|closed|open) ;;
    *) echo ">>   $SLUG: invalid MARKING '$MARKING' in store.conf — upload skipped" >&2; return ;;
  esac
  if [ -z "$DESCRIPTION" ]; then
    echo ">>   $SLUG: no DESCRIPTION in store.conf — upload skipped (a description is required)" >&2
    return
  fi
  echo ">>   uploading '$SLUG' v$pver abi$PLUGIN_ABI (marking=$MARKING) to store"
  # shellcheck disable=SC2086
  "$PYTHON" "$PLUGSTORE_PY" \
    --url "$PLUGSTORE_URL" --token "$PLUGSTORE_TOKEN" \
    --slug "$SLUG" --name "$DISPLAY_NAME" --version "$pver" \
    --platform "$PLATFORM" --arch "$ARCH" --abi "$PLUGIN_ABI" --abi-max "${ABI_MAX:-0}" --file "$zip" \
    --marking "$MARKING" --summary "$SUMMARY" --description "$DESCRIPTION" --depends "${DEPENDS:-}" $EXTRA \
    || echo ">>   $SLUG: store upload FAILED (non-fatal)" >&2
}

# plugin_version <plugin-dir> — the plugin's OWN version (independent of bsdrX), from
# <plugin-dir>/VERSION; falls back to the bsdrX version with a warning if that file is absent.
plugin_version() {
  local d="$1" pv=""
  [ -f "$d/VERSION" ] && pv="$(tr -d '[:space:]' < "$d/VERSION" 2>/dev/null)"
  if [ -z "$pv" ]; then
    echo ">>   $(basename "$d"): no VERSION file — using bsdrX version $VERSION" >&2
    pv="$VERSION"
  fi
  printf '%s' "$pv"
}

# nothing to do if the private plugins/ tree isn't present (public snapshot)
shopt -s nullglob
pdirs=("$SRC"/plugins/*/)
if [ ${#pdirs[@]} -eq 0 ]; then
  echo ">> plugins: none in $SRC/plugins — skipping ($PLATFORM/$ARCH)"
  exit 0
fi
command -v zip >/dev/null 2>&1 || { echo ">> plugins: zip not found — skipping ($PLATFORM/$ARCH)" >&2; exit 0; }

# mingw ignores -fPIC (everything is position-independent); keep it off there to avoid a warning.
PIC="-fPIC"; [ "$PLATFORM" = windows ] && PIC=""

mkdir -p "$OUT"
made=0
for d in "${pdirs[@]}"; do
  name="$(basename "$d")"
  # PLUGIN_ONLY (optional) restricts the build to a single plugin, matched by directory name OR its
  # store.conf SLUG (so --plugin legacy-mic and --plugin legacy_mic both work).
  if [ -n "${PLUGIN_ONLY:-}" ]; then
    only_slug="$(sed -n 's/^SLUG=["'\'']*\([^"'\'' ]*\).*/\1/p' "$d/store.conf" 2>/dev/null | head -1)"
    [ "$name" = "$PLUGIN_ONLY" ] || [ "$only_slug" = "$PLUGIN_ONLY" ] || continue
  fi
  srcs=("$d"*.c)
  [ ${#srcs[@]} -gt 0 ] || { echo ">>   $name: no .c — skipped"; continue; }
  # Optional per-plugin platform allowlist (store.conf PLATFORMS="linux windows macos"): skip this
  # plugin entirely for a platform not in the list — no build, no package, no store upload. Empty or
  # absent = every platform (the default). Media-effect plugins hook the DESKTOP capture/mic path
  # (capture.c/micsub.c), which Android's Kotlin/JNI media pipeline never invokes, so they set this to
  # the desktop trio rather than ship a dead .so to the Android store. Read via a subshell so the rest
  # of store.conf (SLUG/MARKING/…) isn't pulled into this scope early.
  plats="$(sh -c '. "$1" >/dev/null 2>&1; printf %s "${PLATFORMS:-}"' _ "$d/store.conf" 2>/dev/null || true)"
  if [ -n "$plats" ] && ! printf ' %s ' "$plats" | grep -q " $PLATFORM "; then
    echo ">>   $name: not built for $PLATFORM (store.conf PLATFORMS='$plats')"
    continue
  fi
  pver="$(plugin_version "$d")"
  # Optional per-plugin build config (sourced): a media plugin declares extra compile flags + link libs
  # here — e.g. ONNX. It may reference env the caller exports per platform (ONNX_PREFIX, sysroots, …).
  # PLUGIN_BUILD_CFLAGS is added to the compile; PLUGIN_BUILD_LIBS to the link. A plugin with none builds
  # exactly as before (libc only).
  PLUGIN_BUILD_CFLAGS=""; PLUGIN_BUILD_LIBS=""
  if [ -f "$d/build.conf" ]; then
    # shellcheck disable=SC1090
    . "$d/build.conf"
  fi
  stage="$(mktemp -d)/$name"; mkdir -p "$stage"
  echo ">>   building plugin '$name' v$pver (abi$PLUGIN_ABI) for $PLATFORM/$ARCH${PLUGIN_BUILD_LIBS:+ (+libs)}"
  # shellcheck disable=SC2086
  if ! $PLUGIN_CC -O2 $PIC -shared $PLUGIN_CFLAGS $PLUGIN_BUILD_CFLAGS -I"$SRC/include" "${srcs[@]}" \
        $PLUGIN_BUILD_LIBS -o "$stage/$name$PLUGIN_EXT"; then
    echo ">>   $name: build FAILED for $PLATFORM/$ARCH" >&2; rm -rf "$(dirname "$stage")"; continue
  fi
  [ -f "$d/README.md" ] && cp -p "$d/README.md" "$stage/README.md"
  [ -f "$d/LICENSE" ]   && cp -p "$d/LICENSE"   "$stage/LICENSE"   # ship the plugin's own license (e.g. BSD)
  cat > "$stage/INSTALL.txt" <<EOF
bsdrX plugin '$name' — $PLATFORM/$ARCH build ($name$PLUGIN_EXT). Packaged on its own, apart from the app.

Install: drop $name$PLUGIN_EXT into a directory the agent scans at startup, one of:
  * \$BSDR_PLUGIN_DIR      (export it to point at that folder), or
  * <install-prefix>/lib/bsdrX/plugins/ , or
  * ./build/plugins/       (when running from a source checkout).

This plugin may be DANGEROUS (see README.md). It is NOT part of the normal bsdrX distribution and is
loaded only when present in a scanned directory. Use the build that matches your agent's OS+arch.
EOF
  z="$OUT/bsdrX-plugin-$name-$PLATFORM-$ARCH-$pver-abi$PLUGIN_ABI.zip"
  rm -f "$z"
  ( cd "$(dirname "$stage")" && zip -qr "$z" "$name" )
  rm -rf "$(dirname "$stage")"
  echo ">>   packaged $(basename "$z")"
  [ "$PUSH" = 1 ] && store_push "$d" "$name" "$z" "$pver"
  made=$((made+1))
done
echo ">> plugins: $made packaged for $PLATFORM/$ARCH -> $OUT"
