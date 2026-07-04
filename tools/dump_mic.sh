#!/usr/bin/env bash
# dump_mic.sh — capture the Quest microphone and decode its on-wire format, for LAN or cloud.
#
# Purpose: confirm the mic frame format so bsdrX decodes it correctly.
#   LAN   : Quest -> PC on UDP 45003 (same bidirectional socket as desktop-audio-out).
#           ASSUMED format: [Opus payload XOR-0x14][8B trailer: u32 ssrc LE | u32 seq LE].
#   cloud : relay -> PC on the room audio port, PLAIN RTP pt 100 (no XOR), Opus payload.
#
# USAGE:
#   tools/dump_mic.sh -m lan   [-i IFACE] [-q QUEST_IP] [-p 45003] [-d 15] [-o OUTDIR]
#   tools/dump_mic.sh -m cloud [-i IFACE] [-H RELAY_IP] [-p AUDIOPORT] [-d 15] [-o OUTDIR]
#
#   -m lan|cloud   which mic path (required)
#   -i IFACE       capture interface (default: auto = default route iface; LAN may need the LAN iface)
#   -q QUEST_IP    LAN mode: the Quest's IP (mic source). Default: auto (busiest private peer on the port)
#   -H RELAY_IP    cloud mode: the relay IP (mic source). From the "rooms: relay X" debug.log line
#   -p PORT        UDP port (LAN default 45003; cloud = the audio= port from the debug.log rooms line)
#   -d SECONDS     capture duration (default 15) — TALK INTO THE HEADSET during this window
#   -o OUTDIR      output dir (default ./mic_dump_<pid>)
#
# While it captures: put the headset on and TALK. Then it prints, for the first packets from the
# mic source, the length / trailer / de-XOR'd Opus TOC (mono-vs-stereo, frame size) so we can see
# whether the assumed format is right. Raw Opus frames are also written to OUTDIR for offline decode.
#
# Requires: tcpdump (or dumpcap) + tshark + python3.
set -u

MODE=""; IFACE=""; QIP=""; RELAY=""; PORT=""; DUR=15; OUT=""; INPCAP=""
while getopts "m:i:q:H:p:d:o:r:h" o; do case "$o" in
  m) MODE=$OPTARG;; i) IFACE=$OPTARG;; q) QIP=$OPTARG;; H) RELAY=$OPTARG;;
  p) PORT=$OPTARG;; d) DUR=$OPTARG;; o) OUT=$OPTARG;; r) INPCAP=$OPTARG;;
  h) sed -n '2,33p' "$0"; exit 0;; *) echo "see -h"; exit 2;; esac; done
# -r PCAP : analyse an EXISTING capture (e.g. taken with Wireshark on Windows) instead of capturing.

[ "$MODE" = lan ] || [ "$MODE" = cloud ] || { echo "ERROR: -m lan|cloud required"; exit 2; }
command -v tshark  >/dev/null || { echo "ERROR: tshark not found (apt install tshark)"; exit 1; }
command -v python3 >/dev/null || { echo "ERROR: python3 not found"; exit 1; }
[ -z "$IFACE" ] && IFACE="$(ip route show default 2>/dev/null | awk '/default/{print $5; exit}')"
[ -z "$IFACE" ] && IFACE="any"
[ -z "$PORT" ] && { [ "$MODE" = lan ] && PORT=45003 || { echo "ERROR: cloud needs -p AUDIOPORT (audio= in the rooms log line)"; exit 2; }; }
[ "$MODE" = cloud ] && [ -z "$RELAY" ] && { echo "ERROR: cloud needs -H RELAY_IP (rooms: relay ... in debug.log)"; exit 2; }
[ -z "$OUT" ] && OUT="./mic_dump_$$"; mkdir -p "$OUT"; PCAP="$OUT/mic.pcap"

if [ -n "$INPCAP" ]; then
  [ -s "$INPCAP" ] || { echo "ERROR: -r pcap not found: $INPCAP"; exit 1; }
  PCAP="$INPCAP"
  echo "== analysing existing capture: $PCAP (port $PORT, $MODE) =="
else
  echo "== capturing $DUR s on $IFACE, udp port $PORT ($MODE) =="
  echo "   >>> PUT THE HEADSET ON AND TALK NOW <<<"
  if command -v dumpcap >/dev/null; then
    dumpcap -i "$IFACE" -a "duration:$DUR" -f "udp port $PORT" -w "$PCAP" >/dev/null 2>&1
  else
    timeout "$DUR" tcpdump -i "$IFACE" -n -w "$PCAP" "udp port $PORT" >/dev/null 2>&1
  fi
  [ -s "$PCAP" ] || { echo "ERROR: empty capture. Wrong interface? (try -i <your LAN iface>)"; exit 1; }
fi

# Identify the mic SOURCE (Quest on LAN, relay on cloud)
SRC="$QIP"; [ "$MODE" = cloud ] && SRC="$RELAY"
if [ -z "$SRC" ]; then   # LAN auto: the private peer (not us) sending the most packets to PORT
  SRC=$(tshark -r "$PCAP" -Y "udp.dstport==$PORT" -T fields -e ip.src 2>/dev/null \
        | grep -E '^(10\.|192\.168\.|172\.)' | sort | uniq -c | sort -rn | awk 'NR==1{print $2}')
  echo "   auto-detected mic source (Quest) = ${SRC:-<none, talk louder / check iface>}"
fi
[ -z "$SRC" ] && { echo "ERROR: no inbound packets to :$PORT — Quest not sending mic, or wrong iface/port."; exit 1; }

# Direction stats so it's obvious which way audio flows
echo; echo "== packet counts on :$PORT =="
tshark -r "$PCAP" -q -z conv,udp 2>/dev/null | awk '/<->/{print "   "$0}' | grep -E "\.$PORT" | head
N=$(tshark -r "$PCAP" -Y "ip.src==$SRC && udp.dstport==$PORT" 2>/dev/null | wc -l)
echo "   $N packets from mic source $SRC -> :$PORT"
[ "$N" -eq 0 ] && { echo "   (none — the Quest sent no mic. Was the headset mic active / unmuted?)"; }

# Dump payload hex of inbound packets and decode the format
tshark -r "$PCAP" -Y "ip.src==$SRC && udp.dstport==$PORT" -T fields -e udp.payload 2>/dev/null \
  | grep -v '^$' > "$OUT/payloads.hex"

echo; echo "== format analysis (first 12 packets from $SRC) =="
MODE="$MODE" python3 - "$OUT/payloads.hex" "$OUT/opus.raw" <<'PY'
import sys, os
mode = os.environ.get("MODE","lan")
inp, rawout = sys.argv[1], sys.argv[2]
def toc_str(t):
    cfg=(t>>3)&0x1f; s=(t>>2)&1; c=t&3
    bw=["NB","NB","NB","NB","MB","MB","MB","MB","WB","WB","WB","WB",
        "SWB","SWB","FB","FB","NB","NB","NB","NB","WB","WB","WB","WB",
        "SWB","SWB","SWB","SWB","FB","FB","FB","FB"][cfg]
    mode=("SILK" if cfg<12 else "Hybrid" if cfg<16 else "CELT")
    return f"cfg={cfg}({mode}/{bw}) {'STEREO' if s else 'mono'} framecode={c}"
lines=[l.strip() for l in open(inp) if l.strip()]
raw=open(rawout,"wb"); shown=0
for hx in lines:
    try: b=bytes.fromhex(hx)
    except ValueError: continue
    if mode=="lan":
        if len(b)<=8: continue
        payload, trailer = b[:-8], b[-8:]
        ssrc=int.from_bytes(trailer[0:4],"little"); seq=int.from_bytes(trailer[4:8],"little")
        opus=bytes(x^0x14 for x in payload)          # de-XOR
        toc=opus[0]
        if shown<12:
            print(f"  len={len(b):4d} trailer ssrc={ssrc:#010x} seq={seq:<6d} deXOR-TOC={toc:#04x} {toc_str(toc)}")
        raw.write(len(opus).to_bytes(2,'little')+opus) # length-prefixed for offline opus decode
    else:  # cloud: plain RTP
        if len(b)<13: continue
        pt=b[1]&0x7f; seq=int.from_bytes(b[2:4],"big"); ssrc=int.from_bytes(b[8:12],"big")
        opus=b[12:]; toc=opus[0] if opus else 0
        if shown<12:
            print(f"  len={len(b):4d} RTP pt={pt} seq={seq:<6d} ssrc={ssrc:#010x} TOC={toc:#04x} {toc_str(toc)}")
        raw.write(len(opus).to_bytes(2,'little')+opus)
    shown+=1
raw.close()
print(f"\n  analysed {shown} packets; raw length-prefixed Opus -> {rawout}")
if mode=="lan":
    print("  EXPECT: a stable ssrc, incrementing seq, and a VALID Opus TOC after de-XOR.")
    print("  If de-XOR-TOC looks random / ssrc unstable, the LAN mic is NOT [Opus^0x14][8B trailer].")
else:
    print("  EXPECT: pt=100, stable ssrc, incrementing seq, valid Opus TOC (NO XOR on cloud).")
PY
echo; echo "Saved pcap: $PCAP   payloads: $OUT/payloads.hex   opus: $OUT/opus.raw"
echo "Inspect packets in Wireshark: open $PCAP, Decode As udp.port=$PORT -> RTP (cloud) or look raw (lan)."
