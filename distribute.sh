#!/usr/bin/env bash
# distribute.sh — clean the tree, then rebuild EVERY redistributable bundle into dist/:
#
#   dist/bsdrX.zip          Linux    AppImage + .deb     docker image  bsdrx-linux-deps
#   dist/bsdrX-win.zip      Windows  .exe + FFmpeg DLLs  mingw-w64 (native, WIN_DEPS)
#   dist/bsdrX-osx.zip      macOS    universal .app      docker image  bsdrx-osx-full (osxcross)
#   dist/bsdrX-android.apk  Android  APK                 gradle + NDK (native, ANDROID_HOME)
#
# Usage:
#   ./distribute.sh                 # clean + build all four
#   ./distribute.sh linux windows   # only the named platforms
#   SKIP=osx ./distribute.sh        # build all except osx
#
# Key env overrides:
#   WIN_DEPS   mingw dep prefix (default: ../win-deps or ./win-deps if present)
#   ANDROID_HOME / ANDROID_SDK_ROOT   Android SDK (default: /opt/android-sdk)
#   ANDROID_VARIANT   debug|release (default: debug)
#   OSXCROSS_DIR   docker-osxcross checkout (default: /storage/osxcross/docker-osxcross)
#   NO_CLEAN=1     skip the clean step
#
# Failures on one platform do NOT abort the others; a summary is printed at the end.

set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"
mkdir -p "$DIST"

# ---- config / defaults -------------------------------------------------------
WIN_DEPS="${WIN_DEPS:-}"
[ -z "$WIN_DEPS" ] && for c in "$ROOT/../win-deps" "$ROOT/win-deps"; do [ -d "$c" ] && WIN_DEPS="$(cd "$c" && pwd)" && break; done
ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/android-sdk}}"
ANDROID_VARIANT="${ANDROID_VARIANT:-debug}"
OSXCROSS_DIR="${OSXCROSS_DIR:-/storage/osxcross/docker-osxcross}"
LINUX_IMAGE="${LINUX_IMAGE:-bsdrx-linux-deps}"
OSX_IMAGE="${OSX_IMAGE:-bsdrx-osx-full}"
# Canonical version = the BSDR_VERSION literal in include/bsdr/version.h (single source of truth).
VERSION="$(sed -n 's/.*BSDR_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/include/bsdr/version.h" 2>/dev/null)"
[ -n "$VERSION" ] || VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)"

# ---- pretty output -----------------------------------------------------------
if [ -t 1 ]; then B=$'\e[1m'; DIM=$'\e[2m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; C=$'\e[36m'; Z=$'\e[0m'; else B= DIM= G= Y= R= C= Z=; fi
STEP=0; TOTAL=0
declare -a RESULTS
log()   { printf '%s\n' "$*"; }
banner(){ STEP=$((STEP+1)); printf '\n%s========================================================================%s\n' "$C" "$Z"
          printf '%s[%d/%d] %s%s\n' "$B" "$STEP" "$TOTAL" "$1" "$Z"
          printf '%s========================================================================%s\n' "$C" "$Z"; }
ok()    { printf '  %s✔ %s%s\n' "$G" "$*" "$Z"; }
warn()  { printf '  %s⚠ %s%s\n' "$Y" "$*" "$Z"; }
err()   { printf '  %s✗%s %s\n' "$R" "$Z" "$*"; }
have()  { command -v "$1" >/dev/null 2>&1; }
secs()  { local s=$1; printf '%dm%02ds' $((s/60)) $((s%60)); }

record(){ RESULTS+=("$1|$2|$3"); }   # platform|status|detail
# rename a produced artifact to a version-stamped name in dist/
vmv(){ [ -f "$DIST/$1" ] && mv -f "$DIST/$1" "$DIST/$2" && ok "packaged $2"; }

# ---- platform selection ------------------------------------------------------
ALL=(linux windows osx android relay)
SEL=("$@"); [ ${#SEL[@]} -gt 0 ] || SEL=("${ALL[@]}")
declare -A WANT
for p in "${SEL[@]}"; do WANT["$p"]=1; done
IFS=',' read -ra SKIPS <<< "${SKIP:-}"; for s in "${SKIPS[@]}"; do unset 'WANT[$s]'; done
# count phases: clean + each wanted platform
[ "${NO_CLEAN:-0}" = 1 ] || TOTAL=$((TOTAL+1))
for p in "${ALL[@]}"; do [ -n "${WANT[$p]:-}" ] && TOTAL=$((TOTAL+1)); done

log "${B}bsdrX distribute${Z}  version=${C}$VERSION${Z}"
log "  targets : ${!WANT[*]}"
log "  dist    : $DIST"

# ---- 0. clean ----------------------------------------------------------------
if [ "${NO_CLEAN:-0}" != 1 ]; then
  banner "Clean source tree"
  ( cd "$ROOT" && make clean >/dev/null 2>&1 || true )
  rm -rf "$ROOT"/build "$ROOT"/build-* 2>/dev/null || true
  if [ -x "$ROOT/android/gradlew" ]; then ( cd "$ROOT/android" && ANDROID_HOME="$ANDROID_HOME" ./gradlew --no-daemon --quiet clean >/dev/null 2>&1 || true ); fi
  # drop legacy UNVERSIONED bundle names so dist/ ends up with only versioned artifacts
  rm -f "$DIST"/bsdrX.zip "$DIST"/bsdrX-win.zip "$DIST"/bsdrX-osx.zip \
        "$DIST"/bsdrX-android.apk "$DIST"/bsdrX-x86_64.AppImage 2>/dev/null || true
  ok "cleaned build*/, android build, and stale unversioned bundles"
fi

# ---- helpers -----------------------------------------------------------------
# Try to bring the Docker daemon up if it's installed but not running. Handles the
# common launchers (systemd, SysV service, open-source `dockerd`) and, if we lack
# permission, retries the privileged ones under sudo. Then polls until the socket
# answers. Result is cached so the linux+osx builds only attempt the start once.
DOCKER=""          # resolved docker invocation: "docker" or "sudo docker" (empty until probed)
DOCKER_STATE=""    # "" = re-probe, "up" = confirmed (never cached as down; each step re-probes)

# Find a docker invocation that actually reaches the daemon and remember it in $DOCKER. Try the
# plain command first; if that fails, fall back to `sudo docker` — on hosts where the user isn't in
# the docker group the daemon is up but only root can talk to it. sudo prompts for the password if
# it needs one (creds are cached, so the prompt appears at most once). Returns 0 and sets DOCKER.
docker_probe(){
  have docker || return 1
  if docker info >/dev/null 2>&1; then DOCKER="docker"; return 0; fi
  have sudo || return 1
  if ! sudo -n true 2>/dev/null; then      # no cached sudo creds -> prompt (visible) once
    log "  docker needs root — enter your sudo password if prompted"
    sudo -v || return 1
  fi
  if sudo docker info >/dev/null 2>&1; then DOCKER="sudo docker"; return 0; fi
  return 1
}

docker_start(){
  have docker || { warn "docker binary not found"; return 1; }
  log "  docker daemon not responding — attempting to start it…"
  local sudo=""; [ "$(id -u)" -ne 0 ] && have sudo && sudo="sudo -n"
  # ordered attempts: rootless/user daemon first (no sudo), then system launchers.
  # Only try a launcher whose tool actually exists on this host.
  local tried=0
  have systemctl && { log "  trying: ${DIM}systemctl --user start docker${Z}"; systemctl --user start docker >/dev/null 2>&1 && tried=1; }
  [ "$tried" = 1 ] || { have systemctl && { log "  trying: ${DIM}${sudo:+$sudo }systemctl start docker${Z}"; $sudo systemctl start docker >/dev/null 2>&1 && tried=1; }; }
  [ "$tried" = 1 ] || { have service   && { log "  trying: ${DIM}${sudo:+$sudo }service docker start${Z}";   $sudo service docker start   >/dev/null 2>&1 && tried=1; }; }
  [ "$tried" = 1 ] || { have rc-service && { log "  trying: ${DIM}${sudo:+$sudo }rc-service docker start${Z}"; $sudo rc-service docker start >/dev/null 2>&1 && tried=1; }; }
  # last resort: launch dockerd directly in the background (rootless or as root)
  if [ "$tried" != 1 ] && have dockerd; then
    log "  trying: ${DIM}${sudo:+$sudo }dockerd (background)${Z}"
    $sudo dockerd >/dev/null 2>&1 &
    tried=1
  fi
  [ "$tried" = 1 ] || { err "no way to start docker (no systemctl/service/dockerd, or sudo needs a password)"; return 1; }
  # poll for the socket to come alive (daemons take a moment to open it)
  local i
  for i in $(seq 1 30); do
    docker_probe && { ok "docker daemon is up"; return 0; }
    sleep 1
  done
  err "docker daemon did not come up within 30s"
  return 1
}
docker_ok(){
  [ "$DOCKER_STATE" = up ] && return 0
  have docker || { warn "docker binary not found"; return 1; }
  # Probe a few times before giving up: try plain docker, then sudo docker, retrying because the
  # daemon can be slow to answer (freshly started or busy) and a single probe races it.
  local i
  for i in 1 2 3 4 5; do
    docker_probe && { DOCKER_STATE=up; return 0; }
    sleep 1
  done
  if docker_start; then DOCKER_STATE=up; return 0; fi
  # Do NOT cache 'down': a later bundle may find the daemon up (it may still be coming up, or
  # the operator may start it between steps), so every docker step re-probes independently.
  return 1
}

# =============================================================================
# Linux : AppImage + deb  (self-contained docker image)
# =============================================================================
build_linux(){
  banner "Linux bundle  ->  dist/bsdrX.zip"
  local t0=$SECONDS
  if ! docker_ok; then err "docker not available/running — cannot build the Linux bundle"; record linux SKIP "docker unavailable"; return; fi
  if ! $DOCKER image inspect "$LINUX_IMAGE" >/dev/null 2>&1; then
    log "  building docker image ${C}$LINUX_IMAGE${Z} (first run: compiles a minimal ffmpeg — several minutes)…"
    if ! $DOCKER build -f "$ROOT/scripts/linux-bundle.Dockerfile" -t "$LINUX_IMAGE" "$ROOT/scripts"; then
      err "image build failed"; record linux FAIL "image build"; return; fi
    ok "image $LINUX_IMAGE built"
  else ok "reusing image $LINUX_IMAGE"; fi
  log "  running build-linux-bundle.sh in container…"
  if $DOCKER run --rm -e VERSION="$VERSION" -v "$ROOT":/src:ro -v "$DIST":/out "$LINUX_IMAGE" \
        bash /src/scripts/build-linux-bundle.sh; then
    vmv "bsdrX.zip" "bsdrX-$VERSION.zip"
    vmv "bsdrX-x86_64.AppImage" "bsdrX-$VERSION-x86_64.AppImage"
    record linux OK "$(secs $((SECONDS-t0)))"
  else err "build-linux-bundle.sh failed"; record linux FAIL "bundle script"; fi
}

# =============================================================================
# Windows : exe + FFmpeg DLLs  (mingw-w64, native)
# =============================================================================
build_windows(){
  banner "Windows bundle  ->  dist/bsdrX-win.zip"
  local t0=$SECONDS
  if ! have x86_64-w64-mingw32-gcc; then err "mingw-w64 not installed (x86_64-w64-mingw32-gcc)"; record windows SKIP "no mingw"; return; fi
  if [ -z "$WIN_DEPS" ] || [ ! -d "$WIN_DEPS" ]; then err "WIN_DEPS not found (set WIN_DEPS to the mingw dep prefix)"; record windows SKIP "no WIN_DEPS"; return; fi
  ok "mingw + WIN_DEPS=$WIN_DEPS"
  if WIN_DEPS="$WIN_DEPS" OUT="$DIST" SRC="$ROOT" VERSION="$VERSION" bash "$ROOT/scripts/build-win-bundle.sh"; then
    vmv "bsdrX-win.zip" "bsdrX-win-$VERSION.zip"
    record windows OK "$(secs $((SECONDS-t0)))"
  else err "build-win-bundle.sh failed"; record windows FAIL "bundle script"; fi
}

# =============================================================================
# macOS : universal .app  (osxcross docker image)
# =============================================================================
build_osx(){
  banner "macOS bundle  ->  dist/bsdrX-osx.zip"
  local t0=$SECONDS
  if ! docker_ok; then err "docker not available/running — cannot build the macOS bundle"; record osx SKIP "docker unavailable"; return; fi
  if ! $DOCKER image inspect "$OSX_IMAGE" >/dev/null 2>&1; then
    # need an osxcross base first
    if ! $DOCKER image inspect osxcross:local >/dev/null 2>&1; then
      if [ -d "$OSXCROSS_DIR" ]; then
        log "  building osxcross base image (docker buildx bake in $OSXCROSS_DIR)…"
        if ! ( cd "$OSXCROSS_DIR" && $DOCKER buildx bake ); then err "osxcross base build failed"; record osx FAIL "osxcross base"; return; fi
      else
        err "no osxcross:local image and no $OSXCROSS_DIR — set OSXCROSS_DIR to your docker-osxcross checkout"; record osx SKIP "no osxcross"; return
      fi
    fi
    log "  building docker image ${C}$OSX_IMAGE${Z} (cross-builds darwin openssl/opus/srtp2/usrsctp/pcap)…"
    if ! $DOCKER build -f "$ROOT/scripts/osx-bundle.Dockerfile" -t "$OSX_IMAGE" "$ROOT/scripts"; then
      err "image build failed"; record osx FAIL "image build"; return; fi
    ok "image $OSX_IMAGE built"
  else ok "reusing image $OSX_IMAGE"; fi
  log "  running build-osx-bundle.sh in container…"
  if $DOCKER run --rm -e VERSION="$VERSION" -v "$ROOT":/src:ro -v "$DIST":/out "$OSX_IMAGE" \
        bash /src/scripts/build-osx-bundle.sh; then
    vmv "bsdrX-osx.zip" "bsdrX-osx-$VERSION.zip"
    record osx OK "$(secs $((SECONDS-t0)))"
  else err "build-osx-bundle.sh failed"; record osx FAIL "bundle script"; fi
}

# =============================================================================
# Android : APK  (gradle + NDK, native)
# =============================================================================
build_android(){
  banner "Android APK  ->  dist/bsdrX-android.apk"
  local t0=$SECONDS
  if [ ! -x "$ROOT/android/gradlew" ]; then err "android/gradlew missing"; record android SKIP "no gradlew"; return; fi
  if [ ! -d "$ANDROID_HOME" ]; then err "Android SDK not found at $ANDROID_HOME (set ANDROID_HOME)"; record android SKIP "no SDK"; return; fi
  ok "Android SDK=$ANDROID_HOME  variant=$ANDROID_VARIANT"
  local task apk
  case "$ANDROID_VARIANT" in
    release) task=assembleRelease; apk="$ROOT/android/app/build/outputs/apk/release/app-release.apk";;
    *)       task=assembleDebug;   apk="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk";;
  esac
  log "  running gradle $task (first run downloads the NDK toolchain)…"
  if ( cd "$ROOT/android" && ANDROID_HOME="$ANDROID_HOME" ANDROID_SDK_ROOT="$ANDROID_HOME" ./gradlew --no-daemon "$task" ); then
    if [ -f "$apk" ]; then cp -f "$apk" "$DIST/bsdrX-android-$VERSION.apk"; ok "packaged bsdrX-android-$VERSION.apk"; record android OK "$(secs $((SECONDS-t0)))";
    else err "APK not produced at $apk"; record android FAIL "no apk"; fi
  else err "gradle $task failed"; record android FAIL "gradle"; fi
}

# =============================================================================
# Relay : static bsdr_micrelay binaries for router + PC arches (zig cross)
# =============================================================================
build_relay(){
  banner "Relay bundle  ->  dist/bsdrX_relay.zip"
  local t0=$SECONDS
  if ! have curl; then err "curl not found (needed to fetch the zig toolchain)"; record relay SKIP "no curl"; return; fi
  if ! have zip;  then err "zip not found"; record relay SKIP "no zip"; return; fi
  log "  cross-building static bsdr_micrelay (amd64/x86/arm64/armv7/mipsel/mips) via zig — slow first run…"
  if SRC="$ROOT" OUT="$DIST" bash "$ROOT/scripts/build-micrelay.sh"; then
    record relay OK "$(secs $((SECONDS-t0)))"
  else err "build-micrelay.sh failed"; record relay FAIL "relay script"; fi
}

# ---- run selected platforms --------------------------------------------------
[ -n "${WANT[linux]:-}"   ] && build_linux
[ -n "${WANT[windows]:-}" ] && build_windows
[ -n "${WANT[osx]:-}"     ] && build_osx
[ -n "${WANT[android]:-}" ] && build_android
[ -n "${WANT[relay]:-}"   ] && build_relay

# ---- summary -----------------------------------------------------------------
printf '\n%s========================================================================%s\n' "$C" "$Z"
printf '%sSummary%s  (version %s)\n' "$B" "$Z" "$VERSION"
printf '%s========================================================================%s\n' "$C" "$Z"
fail=0
for r in "${RESULTS[@]}"; do
  IFS='|' read -r plat status detail <<< "$r"
  case "$status" in
    OK)   printf '  %s✔%s  %-8s %s\n' "$G" "$Z" "$plat" "$detail";;
    SKIP) printf '  %s⚠%s  %-8s skipped: %s\n' "$Y" "$Z" "$plat" "$detail";;
    *)    printf '  %s✗%s  %-8s FAILED: %s\n' "$R" "$Z" "$plat" "$detail"; fail=1;;
  esac
done
printf '\n%sArtifacts in %s:%s\n' "$B" "$DIST" "$Z"
for f in "bsdrX-$VERSION.zip" "bsdrX-win-$VERSION.zip" "bsdrX-osx-$VERSION.zip" "bsdrX-android-$VERSION.apk" "bsdrX-$VERSION-x86_64.AppImage" bsdr-agent_"${VERSION#v}"_amd64.deb bsdrX_relay.zip; do
  [ -f "$DIST/$f" ] && printf '  %-28s %s\n' "$f" "$(du -h "$DIST/$f" | cut -f1)"
done
exit $fail
