#!/usr/bin/env bash
# build-micrelay.sh — build STATIC bsdr_micrelay binaries for router + PC arches and package them
# into dist/bsdrX_relay.zip.
#
# The router companion (tools/bsdr_micrelay.c) captures the headset's mic RTP on the router and
# forwards it to bsdr_agent --sniff-remote. Routers are arm/mips/etc., so we cross-compile with
# `zig cc` (one self-contained toolchain for every target) and a from-source, dependency-free
# STATIC libpcap (no dbus/bluetooth/usb/libnl) — so each binary is a single scp-and-run file.
#
# Env: SRC (bsdrX root, default the repo this lives in), OUT (default $SRC/dist),
#      ZIG (zig binary; auto-downloaded to $CACHE if absent), ARCHES (space-separated subset).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-$(cd "$HERE/.." && pwd)}"
OUT="${OUT:-$SRC/dist}"
CACHE="${CACHE:-$SRC/.relaycache}"
ZIG_VER=0.13.0
PCAP_VER=1.10.5
mkdir -p "$OUT" "$CACHE"

# arch label  ->  zig target triple
ALL_ARCHES="amd64:x86_64-linux-musl x86:x86-linux-musl arm64:aarch64-linux-musl \
            armv7:arm-linux-musleabihf mipsel:mipsel-linux-musl mips:mips-linux-musl"
ARCHES="${ARCHES:-$ALL_ARCHES}"

log(){ printf '>> %s\n' "$*"; }

# ---- 1. zig (self-contained cross toolchain) --------------------------------
ZIG="${ZIG:-$CACHE/zig/zig}"
if [ ! -x "$ZIG" ]; then
  log "fetching zig $ZIG_VER"
  curl -fsSL "https://ziglang.org/download/$ZIG_VER/zig-linux-x86_64-$ZIG_VER.tar.xz" -o "$CACHE/zig.tar.xz" || { echo "zig download failed"; exit 1; }
  mkdir -p "$CACHE/zig"; tar -xJf "$CACHE/zig.tar.xz" -C "$CACHE/zig" --strip-components=1
  ZIG="$CACHE/zig/zig"
fi
export ZIG_GLOBAL_CACHE_DIR="$CACHE/zig-cache"

# ---- 2. libpcap source ------------------------------------------------------
if [ ! -d "$CACHE/libpcap-$PCAP_VER" ]; then
  log "fetching libpcap $PCAP_VER"
  curl -fsSL "https://www.tcpdump.org/release/libpcap-$PCAP_VER.tar.gz" -o "$CACHE/libpcap.tar.gz" || { echo "libpcap download failed"; exit 1; }
  tar -xzf "$CACHE/libpcap.tar.gz" -C "$CACHE"
fi

# ---- 3. per-arch: static libpcap + static micrelay --------------------------
STAGE="$(mktemp -d)"; mkdir -p "$STAGE/bsdr_micrelay"
cp "$SRC/README.md" "$SRC/LICENSE.md" "$STAGE/bsdr_micrelay/" 2>/dev/null || true
cat > "$STAGE/bsdr_micrelay/README-relay.txt" <<EOF
bsdr_micrelay — router-side owner-mic relay for bsdrX.

Pick the binary for your router's CPU and copy it there:
  bsdr_micrelay-arm64    64-bit ARM (most modern routers, RPi)
  bsdr_micrelay-armv7    32-bit ARM
  bsdr_micrelay-mipsel   little-endian MIPS (many OpenWRT routers)
  bsdr_micrelay-mips     big-endian MIPS
  bsdr_micrelay-amd64    64-bit x86
  bsdr_micrelay-x86      32-bit x86
Each is fully static (no libpcap/glibc needed on the router).

Auto mode (recommended) — zero config, serves every paired headset/agent in parallel:
  router:  ./bsdr_micrelay-<arch> --iface br-lan
  PC:      bsdr_agent --sniff-remote 45099   (or just enable the relay owner-mic method in the web panel)
The relay beacons; each PC finds it and registers for the headset it is paired with. The relay only
forwards a headset's mic to the agent it observed paired with it (bind-to-owner), so nobody can
siphon someone else's owner voice.

Static single flow (no discovery/auth), if you prefer to pin one headset:
  router:  ./bsdr_micrelay-<arch> --iface br-lan --quest <headset-ip> --to <pc-ip>:45099
(OpenWRT users: an .ipk recipe is in openwrt/bsdr-micrelay/.)
EOF

built=""
for pair in $ARCHES; do
  name="${pair%%:*}"; triple="${pair##*:}"
  log "=== $name ($triple) ==="
  lpdir="$CACHE/lp-$name"
  if [ ! -f "$lpdir/libpcap.a" ]; then
    rm -rf "$lpdir"; cp -r "$CACHE/libpcap-$PCAP_VER" "$lpdir"
    ( cd "$lpdir" && CC="$ZIG cc -target $triple" AR="$ZIG ar" RANLIB="$ZIG ranlib" \
        ./configure --host="$triple" --with-pcap=linux --disable-shared \
          --disable-dbus --disable-rdma --disable-bluetooth --disable-usb --without-libnl \
          >configure.log 2>&1 && \
      CC="$ZIG cc -target $triple" AR="$ZIG ar" RANLIB="$ZIG ranlib" make -j4 libpcap.a >make.log 2>&1 ) \
      || { echo "   libpcap build failed for $name (see $lpdir/*.log)"; continue; }
  fi
  bin="$STAGE/bsdr_micrelay/bsdr_micrelay-$name"
  # -I"$lpdir" so the freshly built libpcap's pcap.h is found.
  if "$ZIG" cc -target "$triple" -O2 -static -Wl,-s -I"$SRC/include" -I"$SRC/src" -I"$lpdir" -DBSDR_HAVE_PCAP=1 \
       "$SRC/tools/bsdr_micrelay.c" "$SRC/src/micsniff_capture.c" "$lpdir/libpcap.a" -o "$bin" 2>"$CACHE/mr-$name.log"; then
    log "   built $(basename "$bin") ($(wc -c <"$bin") bytes)"
    built="$built $name"
  else
    echo "   micrelay link failed for $name (see $CACHE/mr-$name.log)"; head -5 "$CACHE/mr-$name.log"
  fi
done

if [ -z "$built" ]; then echo "!! no relay binaries built"; rm -rf "$STAGE"; exit 1; fi

# ---- 4. package -------------------------------------------------------------
Z="$OUT/bsdrX_relay.zip"; rm -f "$Z"
( cd "$STAGE" && zip -q -r "$Z" bsdr_micrelay )
rm -rf "$STAGE"
log "relay bundle ->$Z"; ( cd "$OUT" && ls -la bsdrX_relay.zip )
log "arches:$built"
