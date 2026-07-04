#!/usr/bin/env bash
# cloud_capture_diff.sh — capture bsdrX's own cloud share against the live relay and report
# exactly what the relay returns (the official host won't run on Linux, so there's no baseline).
#
# WHY: bsdrX's SCTP data-channel INIT is byte-identical to BigSoup's, yet the relay never
# answers us, and our cloud video may be dropped (wrong producer SSRC). This pins down, from
# bsdrX's side alone, whether the relay is silent / sends ICMP / sends INIT-ACK, and whether the
# RTP SSRC we now put on the wire matches djb2(userSessionId).
#
# USAGE:
#   tools/cloud_capture_diff.sh [-i IFACE] [-H RELAY_IP] [-d SECONDS] [-s USER_SESSION_ID] [-o OUTDIR]
#
#   -i IFACE    capture interface (default: auto = default route iface)
#   -H RELAY_IP relay/mediaserver IP (default: auto-detected = external host with most UDP)
#   -d SECONDS  capture duration (default: 25)
#   -s ID       mediaPeer.userSessionId (from GET /rooms) — enables the SSRC = djb2(id) check
#   -o OUTDIR   output dir (default: ./cloud_capture_<timestamp-ish>)
#
# LINUX-ONLY. The official host does NOT run under Wine (Electron stdio + no GPU/display),
# so there is no official baseline to compare against. This captures bsdrX's OWN cloud share
# against the live relay to see exactly what the relay returns to us. During the window:
#   * Quest adds the screen to the room (as usual), then
#   * bsdrX shares to internet (build/bsdr_agent), or auto-share is on.
# The script reports, on the relay's data port and media ports:
#   - the SCTP handshake (INIT / INIT-ACK / COOKIE-ECHO / COOKIE-ACK / DATA) and which side
#     sent each — i.e. whether the relay answers at all and the official client associates;
#   - the RTP payload-type + SSRC per media port;
#   - bsdrX's own SCTP INIT (via build/sctp_init_dump) and the SSRC bsdrX would use;
#   - a side-by-side of the decisive fields.
#
# Requires: tcpdump (or dumpcap) + tshark. No app source needed.
set -u

IFACE=""; RELAY=""; DUR=25; SID=""; OUT=""; INPCAP=""
while getopts "i:H:d:s:o:r:h" o; do case "$o" in
  i) IFACE=$OPTARG;; H) RELAY=$OPTARG;; d) DUR=$OPTARG;; s) SID=$OPTARG;; o) OUT=$OPTARG;; r) INPCAP=$OPTARG;;
  h) sed -n '2,31p' "$0"; exit 0;; *) echo "see -h"; exit 2;; esac; done
# -r PCAP : analyse an EXISTING capture (e.g. the OFFICIAL host taken with Wireshark on Windows).
#           THIS is how to get the baseline: does the relay send INIT-ACK to the official client?

command -v tshark >/dev/null || { echo "ERROR: tshark not found (apt install tshark)"; exit 1; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
[ -z "$IFACE" ] && IFACE="$(ip route show default 2>/dev/null | awk '/default/{print $5; exit}')"
[ -z "$IFACE" ] && IFACE="any"
[ -z "$OUT" ] && OUT="$HERE/cloud_capture_$$"
mkdir -p "$OUT"; PCAP="$OUT/official.pcap"

# ---- bsdrX reference: our own INIT + the SSRC we would use --------------------------------
echo "== bsdrX reference =="
if [ -x "$HERE/build/sctp_init_dump" ]; then
  "$HERE/build/sctp_init_dump" 2>/dev/null | sed -n '/SCTP hdr/,$p' | sed 's/^/  [bsdrX INIT] /' \
    | tee "$OUT/bsdrx_init.txt"
else
  echo "  (build/sctp_init_dump missing — run: make build/sctp_init_dump)"
fi
BSSRC=""
if [ -n "$SID" ]; then
  # SSRC = djb2(userSessionId): h=5381; h = h*33 + byte; 32-bit  (matches BigSoup FUN_180079000)
  BSSRC=$(python3 - "$SID" <<'PY' 2>/dev/null
import sys
h=5381
for ch in sys.argv[1].encode():
    h=(h*33+ch)&0xffffffff
print(h)
PY
)
  echo "  [bsdrX SSRC] djb2('$SID') = ${BSSRC:-<need python3>}"
fi

# ---- capture (or read an existing pcap) ---------------------------------------------------
if [ -n "$INPCAP" ]; then
  [ -s "$INPCAP" ] || { echo "ERROR: -r pcap not found: $INPCAP"; exit 1; }
  PCAP="$INPCAP"
  echo; echo "== analysing existing capture: $PCAP =="
else
  FILT="udp"; [ -n "$RELAY" ] && FILT="udp and host $RELAY"
  echo
  echo "== capturing $DUR s on $IFACE ($FILT) =="
  echo "   -> NOW: have the Quest add the screen, then bsdr_agent shares to internet."
  if command -v dumpcap >/dev/null; then
    dumpcap -i "$IFACE" -a "duration:$DUR" -f "$FILT" -w "$PCAP" >/dev/null 2>&1
  else
    timeout "$DUR" tcpdump -i "$IFACE" -n -w "$PCAP" "$FILT" >/dev/null 2>&1
  fi
  [ -s "$PCAP" ] || { echo "ERROR: empty capture ($PCAP)"; exit 1; }
fi

# ---- auto-detect relay if not given (external host with most UDP bytes) -------------------
if [ -z "$RELAY" ]; then
  RELAY=$(tshark -r "$PCAP" -q -z conv,udp 2>/dev/null \
    | awk '/<->/{print $1, $3, $9}' \
    | sed 's/:[0-9]*//g' \
    | awk '{ip=$2; if (ip !~ /^(10\.|192\.168\.|172\.|255\.|224\.|0\.)/) print ip; else print $1}' \
    | grep -vE '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.|255\.|224\.|0\.)' \
    | sort | uniq -c | sort -rn | awk 'NR==1{print $2}')
  echo "   auto-detected relay = ${RELAY:-<none>}"
fi
[ -n "$RELAY" ] && RELAYF="and host $RELAY" || RELAYF=""

# ---- per-port summary to/from the relay ---------------------------------------------------
echo
echo "== UDP ports in use with relay $RELAY =="
PORTS=$(tshark -r "$PCAP" -Y "udp $RELAYF" -T fields -e udp.dstport -e udp.srcport 2>/dev/null \
  | tr '\t' '\n' | sort -n | uniq -c | sort -rn | awk '$1>3{print $2}' | head -8)
echo "$PORTS" | sed 's/^/   port /'

# ---- SCTP handshake on each candidate port (force decode UDP-as-SCTP) ---------------------
echo
echo "== SCTP handshake (data channel) — is the relay answering? =="
SCTP_NAMES=("DATA(0)" "INIT(1)" "INIT-ACK(2)" "SACK(3)" "" "" "" "" "" "" "COOKIE-ECHO(10)" "COOKIE-ACK(11)")
for P in $PORTS; do
  CT=$(tshark -r "$PCAP" -d "udp.port==$P,sctp" -Y "sctp $RELAYF && udp.port==$P" \
        -T fields -e ip.src -e sctp.chunk_type 2>/dev/null)
  [ -z "$CT" ] && continue
  echo "  --- port $P ---"
  echo "$CT" | awk -v relay="$RELAY" '
    { dir=($1==relay)?"relay->us":"us->relay";
      split($2,a," ");
      for(i in a){ c=a[i];
        n=(c==0?"DATA":c==1?"INIT":c==2?"INIT-ACK":c==3?"SACK":c==10?"COOKIE-ECHO":c==11?"COOKIE-ACK":c==7?"SHUTDOWN":c==6?"ABORT":"chunk"c);
        key=dir" "n; cnt[key]++ } }
    END{ for(k in cnt) printf "     %-26s x%d\n", k, cnt[k] }' | sort
done
echo "  (If you see 'relay->us INIT-ACK', the relay IS answering us — assoc should complete."
echo "   If only 'us->relay INIT' with nothing back, the relay is silent: either it drops our"
echo "   source (NAT/registration) or the data port isn't an SCTP listener. Watch for ICMP too.)"

# ---- RTP PT + SSRC per media port (compare against djb2(userSessionId)) -------------------
echo
echo "== RTP payload-type + SSRC bsdrX puts on the wire (should be pt 111/100, ssrc=djb2(id)) =="
for P in $PORTS; do
  R=$(tshark -r "$PCAP" -d "udp.port==$P,rtp" -Y "rtp $RELAYF && udp.dstport==$P" \
        -T fields -e rtp.p_type -e rtp.ssrc 2>/dev/null | sort -u | head -4)
  [ -z "$R" ] && continue
  echo "  port $P (us->relay):"
  echo "$R" | awk '{printf "     pt=%-4s ssrc=%s\n", $1, $2}'
done
[ -n "$BSSRC" ] && printf "  >>> bsdrX would send ssrc=%s (=0x%x) for djb2('%s')\n" "$BSSRC" "$BSSRC" "$SID"
echo
echo "Saved: $PCAP  (+ $OUT/bsdrx_init.txt)"
echo "Open in Wireshark with: 'Decode As' UDP port -> SCTP / RTP for the relay ports above."
