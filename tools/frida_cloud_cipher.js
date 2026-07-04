// frida_cloud_cipher.js — dump the cloud media cipher from the LIVE official Bigscreen Remote
// Desktop host (Windows). The cloud video/audio payload is encrypted with a CUSTOM keystream
// cipher (no SRTP/AES in BigSoup.dll). This hooks the jrtplib send point to capture the exact
// bytes BigSoup puts on the wire (ciphertext) and the buffer it was handed (plaintext H.264/Opus),
// so we can recover the keystream = plaintext XOR ciphertext and reverse the cipher.
//
// USAGE (on the Windows host, with the official client running and sharing to internet):
//   1) pip install frida-tools
//   2) frida -n BigscreenRemoteDesktop.exe -l frida_cloud_cipher.js   (or the bigsoup-hosting proc)
//      If BigSoup runs in a child/helper process, attach to that PID instead.
//   3) Share desktop to internet for ~5s, then paste me the console output.
//
// RVAs are from BigSoup.dll in ghidra_proj (image base 0x180000000):
//   RTPSession::SendPacket(data,len,pt,mark,tsinc)  @ 0x2cdbd0   (and overload @ 0x2cdd20)
//   cloud video work (H.264 frame in)               @ 0x96e70
// If the build differs, adjust the RVAs (the function near "Creating desktop video connection
// to Mediasoup server" string is the video worker).

'use strict';
const MOD = 'BigSoup.dll';
const base = Module.findBaseAddress(MOD);
if (!base) { console.log('!! ' + MOD + ' not loaded in this process — attach to the one that hosts it'); }

function hexFirst(ptr, len, n) {
  n = Math.min(len, n || 40);
  let out = [];
  for (let i = 0; i < n; i++) out.push(('0' + ptr.add(i).readU8().toString(16)).slice(-2));
  return out.join(' ');
}

// --- the jrtplib send point: `data` here is the FINAL payload (ciphertext on the wire) ---
function hookSend(rva, label) {
  try {
    Interceptor.attach(base.add(rva), {
      onEnter(args) {
        // thiscall: rcx=this(args[0]), rdx=data(args[1]), r8=len(args[2]), r9=pt(args[3])
        const data = args[1];
        const len = args[2].toInt32();
        if (len <= 0 || len > 4000) return;
        const pt = args[3].toInt32() & 0x7f;
        console.log(`[${label}] pt=${pt} len=${len} cipher: ${hexFirst(data, len, 40)}`);
      }
    });
    console.log('hooked ' + label + ' @ ' + base.add(rva));
  } catch (e) { console.log('hook fail ' + label + ': ' + e); }
}

// --- the video worker: try to dump the PLAINTEXT H.264 frame it receives (before cipher) ---
// param layout varies; we dump the buffer pointed at param[0]+0x.. heuristically. If this is
// noisy, the send-point ciphertext alone + a known keyframe (starts 67/68/65) already gives the
// keystream for those packets.
function hookVideoWorker(rva) {
  try {
    Interceptor.attach(base.add(rva), {
      onEnter(args) {
        const p = args[0];
        try {
          const dptr = p.add(0x20).readPointer();
          const dlen = p.add(0x28).readU32();
          if (dlen > 0 && dlen < 200000)
            console.log(`[VIDEO-IN] plain len=${dlen} h264: ${hexFirst(dptr, dlen, 40)}`);
        } catch (e) {}
      }
    });
    console.log('hooked VIDEO-IN @ ' + base.add(rva));
  } catch (e) { console.log('hook fail VIDEO-IN: ' + e); }
}

hookSend(0x2cdbd0, 'SEND');
hookSend(0x2cdd20, 'SEND2');
hookVideoWorker(0x96e70);
console.log('== ready: share desktop to internet for ~5s, then copy the console output ==');
