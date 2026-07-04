# Capturing a working official Bigscreen cloud (internet) session

Goal: record a **working** official Bigscreen Remote Desktop *internet-sharing*
session so we can reverse the cloud signaling + media our agent can't yet do.
Ground-truth capture, like `quest.pcapng` was for LAN.

Good news: we do **not** need to decrypt TLS. The official host (an Electron app)
logs everything at **debug level to its console (stdout)** — including the
**decrypted WebSocket messages** (`logger.info(data)` on every push) and the room
/ screen / `/rooms` data. The **media (RTP to the relay) is plain**, so a normal
pcap shows it. So two artifacts give the whole picture:

1. **The app's console log** (stdout) → the signaling (WS messages, room/screen,
   device registration).
2. **A plain pcap** → the media (relay IP/ports, RTP payload types, SSRC, latch).

You need: the **official Bigscreen Remote Desktop host** (Windows), a Quest with
Bigscreen, and **Wireshark** on the Windows PC.

## A. Capture the app console log (the important one)

1. Fully **quit** any running Bigscreen Remote Desktop host (check the tray).
2. Find the host's exe (often
   `%LOCALAPPDATA%\Programs\<...>\Bigscreen Remote Desktop.exe` or under
   `C:\Program Files`). Note its full path.
3. Open **Command Prompt** and run (adjust the path):
   ```
   mkdir C:\bsdr
   set ELECTRON_ENABLE_LOGGING=1
   "C:\Path\To\Bigscreen Remote Desktop.exe" > C:\bsdr\app-log.txt 2>&1
   ```
   The app launches and its debug log streams into `C:\bsdr\app-log.txt`. Leave
   this window open (closing it quits the app).

   *If `app-log.txt` stays empty* (some Electron builds detach stdout), instead
   launch normally and capture with DebugView: download Sysinternals **DebugView**,
   run it as admin with "Capture Win32" + "Capture Global Win32" enabled, then
   start the app; save DebugView's output. Send that instead.

## B. Capture a plain pcap (for the media)

In a second Command Prompt, start the capture (Wireshark ships `dumpcap`):
```
"C:\Program Files\Wireshark\dumpcap.exe" -D                       (list interfaces)
"C:\Program Files\Wireshark\dumpcap.exe" -i <N> -w C:\bsdr\cloud.pcapng -f "tcp port 443 or udp"
```
(or use the Wireshark GUI: pick your active interface, start). No need to decrypt
— we just want the UDP media flows.

## C. Do a WORKING internet session

1. In the host app: **log in**, then **enable internet sharing** ("Share to
   Internet").
2. On the **Quest**: connect to the PC over the **internet** (cloud, not LAN)
   until the **desktop is actually streaming in VR**. Move some windows for ~30–60 s
   so there's real video.
3. Stop the pcap (Ctrl-C / red square) and quit the app (close its cmd window).

## D. Send me

- `C:\bsdr\app-log.txt`  (or the DebugView output)
- `C:\bsdr\cloud.pcapng`

Copy them where I can read them (e.g. onto the Linux box under `/working/bsrd/`).

## What I'll pull out
- From the **log**: the WS push that announces the room/screen and triggers
  sharing; the exact `systemInfo` / `deviceUniqueIdentifier` (it's
  `node-machine-id`, not the hostname we send); the populated `/rooms` with a real
  `mediaPeer`/`mediaServer`; and *what makes a screen "shareable"* — i.e. why the
  cloud creates a screen for the official host but not for ours.
- From the **pcap**: the relay IP/ports actually used, the RTP payload types,
  SSRC, the comedia latch, and confirmation that media is plain RTP (and the
  data/input channel's real transport).

That's everything blocking the cloud path.
