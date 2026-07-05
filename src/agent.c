/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
/* Bigscreen Remote Desktop — Linux/Windows/macOS PC host agent.
 *
 * Brings up the LAN host side: discovery (45000) + HTTP control (45678) + the
 * web control panel. On /start it streams the desktop to the paired headset
 * using the real Bigscreen LAN wire format — H.264 video on 45002, Opus audio
 * on 45003, and a DTLS input channel on 45004 (mouse/keyboard -> injector).
 * Internet sharing (cloud relay) is driven from the web panel.
 */
#include "bsdr/protocol.h"
#include "bsdr/version.h"
#include "bsdr/log.h"
#include "bsdr/net.h"
#include "bsdr/discovery.h"
#include "bsdr/control.h"
#include "bsdr/inject.h"
#include "bsdr/udp_transport.h"
#include "bsdr/capture.h"
#include "bsdr/filesrc.h"
#include "bsdr/fileaudio.h"
#include "bsdr/audio.h"
#include "bsdr/dtls.h"
#include "bsdr/input_decode.h"
#include "bsdr/events.h"
#include "bsdr/platform.h"
#ifdef BSDR_HAVE_AUDIO
#include <opus/opus.h>
#endif
#include "bsdr/app.h"
#include "bsdr/tls.h"
#include "bsdr/webui.h"
#include "bsdr/agentlib.h"
#include "bsdr/micsniff.h"
#include "bsdr/overlay.h"
#include "bsdr/threed.h"
#include "bsdr/depth.h"
#include "bsdr/model_store.h"
#include "bsdr/faceswap.h"
#include "bsdr/micsub.h"
#include "bsdr/voice.h"
#if defined(BSDR_PLATFORM_ANDROID)
#include "bsdr_android.h"          /* device-mic voice bridge (no sniffer on Android) */
#endif
#include "bsdr/screenshot.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <sys/uio.h>     /* struct iovec for --sendmmsg batching */
#endif

typedef struct {
    bool video, audio;
    const char *video_file;     /* --file: stream an H.264 file instead of the desktop */
    const char *replay_file;    /* --replay: stream captured 45002 frames verbatim (diagnostic) */
    int fps, bitrate;

    bsdr_mutex *lock;
    bsdr_thread *worker;
    volatile int stop;          /* per-session cancel/stop flag */
    char remote_ip[64];
    char src_path[512];         /* stable storage for the file source */
    char cam_left[256];         /* stable storage for the (left) webcam device */
    char cam_right[256];        /* stable storage for the right webcam device (stereo 3D) */
    bsdr_app *app;              /* shared web-UI state */
    volatile int settings_dirty;   /* headset changed resolution/bitrate */
    struct bsdr_overlay *overlay;  /* in-VR voice-command balloon (owned by run(); shared into sessions) */
    struct bsdr_voice *voice;      /* voice->STT->LLM computer control (owned by run()); NULL until armed */
    /* ---- in-VR media bar controls for file streaming (shared across the video + audio + input
     * threads). Edge-triggered seek via a generation counter so every owner applies it once. ---- */
    int file_gpu;                  /* --file-gpu: encode the file on NVENC instead of libx264 */
    volatile int file_mode;        /* streaming a video file (bar active) */
    /* play/pause, volume, and seek for the media bar live in bsdr_app (a->app->file_*) so the
     * LAN threads, the input thread, and the cloud audio sender all share one source of truth. */
} agent_t;

static volatile sig_atomic_t g_running = 1;
/* Tracks whether the local monitor is currently blanked (privacy), so a signal handler can restore
 * it. sig_atomic_t: written by the main loop and read/written by on_sigint. */
static volatile sig_atomic_t g_screen_blanked = 0;
static void screen_set_blank(int on);   /* defined below (platform variant) */
/* First Ctrl-C: ask the main loop to shut down cleanly. Second Ctrl-C: force-quit (in case a
 * blocking call — e.g. the cloud HTTPS poll — is wedged and the loop can't re-check g_running).
 * Either way, restore the screen FIRST so Ctrl-C never leaves the monitor blacked out. */
static void on_sigint(int sig) {
    (void)sig;
    if (g_screen_blanked) {                 /* unblank before anything else, even a wedged loop */
        screen_set_blank(0);
        g_screen_blanked = 0;
    }
    if (!g_running) {                       /* second Ctrl-C: force-quit, but still tidy up first */
        bsdr_audio_cleanup_stale_devices(); /* don't leave bsdr_* PulseAudio modules loaded */
        _exit(130);
    }
    g_running = 0;
}
void bsdr_agent_stop(void) { g_running = 0; }

/* System prompt handed to the LLM for spoken desktop commands. The tool schema
 * (type_text/key/click/scroll/open_app) lives in llm.c; this tells the model how
 * to use it. */
static const char COMPCTL_SYSTEM_PROMPT[] =
    "You control a Linux desktop on behalf of a user speaking through a VR headset. "
    "The user's words arrive transcribed from speech, so expect informal phrasing and "
    "occasional transcription errors — infer intent. Carry out the request by calling the "
    "provided tools: type_text to type, key for key combos (e.g. \"ctrl+t\", \"alt+F4\", "
    "\"enter\"), click with normalized x,y in [0,1], scroll by an amount, and open_app to "
    "launch a program by command name. Prefer keyboard shortcuts over clicking when reliable. "
    "Take the fewest actions that accomplish the goal, then reply with one short sentence "
    "describing what you did. If the request is unclear or unsafe, do nothing and say so.";

/* Appended to the prompt only when vision is enabled. */
static const char COMPCTL_VISION_NOTE[] =
    " You cannot see the screen by default. When the task needs you to look at the desktop "
    "(to find a UI element, read what's shown, or decide where to click), call the "
    "take_screenshot tool first — it attaches the current screen as an image — then act on it. "
    "Don't take a screenshot when the request doesn't require seeing the screen.";

/* micsniff PCM tap -> voice capture buffer (runs on the sniffer thread). */
static void voice_pcm_sink(void *user, const int16_t *pcm, int frames, int channels) {
    bsdr_voice_push_pcm((bsdr_voice *)user, pcm, frames, channels);
}

#if !defined(BSDR_PLATFORM_ANDROID)
/* Local-mic owner source: capture THIS machine's default microphone and feed it to the voice
 * pipeline — the alternative to sniffing the Quest's mic (LAN/MITM/relay), for when you just want to
 * talk into the computer running bsdrX. Cross-platform via bsdr_pa_record_open. */
struct local_mic_ctx { struct bsdr_voice *voice; volatile int stop; };
static void local_mic_main(void *arg) {
    struct local_mic_ctx *c = (struct local_mic_ctx *)arg;
    bsdr_pa *mic = bsdr_pa_record_open(NULL, 1);   /* default input, mono */
    if (!mic) { BSDR_WARN("bsdr.agent", "local mic: cannot open default input"); return; }
    BSDR_INFO("bsdr.agent", "local mic: capturing this computer's microphone as the owner mic");
    int16_t pcm[960];
    while (!c->stop) {
        int n = bsdr_pa_read(mic, pcm, 960);
        if (n > 0) bsdr_voice_push_pcm(c->voice, pcm, n, 1);
        else if (n < 0) break;
    }
    bsdr_pa_close(mic);
}
#endif
/* voice pipeline state -> balloon visual mode. */
static void voice_state_map(void *user, int state) {
    struct bsdr_overlay *o = (struct bsdr_overlay *)user;
    bsdr_overlay_set_listening(o, state == BSDR_VST_LISTENING);
    bsdr_overlay_set_confirm(o,   state == BSDR_VST_CONFIRM);
    bsdr_overlay_set_working(o,    state == BSDR_VST_WORKING);
}
/* voice status/thinking/result line -> balloon feedback bubble + history. */
static void voice_feedback(void *user, const char *text) {
    bsdr_overlay_push_feedback((struct bsdr_overlay *)user, text);
}
/* on-demand desktop screenshot for the vision model. */
static int voice_screenshot(void *user, uint8_t *out, size_t cap) {
    (void)user;
    return bsdr_screenshot_jpeg(1280, out, cap);
}
#if defined(BSDR_PLATFORM_ANDROID)
/* On Android the voice bubble lives in Kotlin, so state + feedback go out over JNI. */
static void voice_state_android(void *user, int state) { (void)user; bsdr_android_emit_voice_state(state); }
static void voice_feedback_android(void *user, const char *text) { (void)user; bsdr_android_emit_voice_feedback(text); }
#endif

/* --- session worker -------------------------------------------------------*/
/* LAN-mode replay: stream captured 45002 frames verbatim to the Quest, no DTLS.
 * Validates the real LAN transport (single port 45002, XOR-0x14 H.264 custom frame,
 * each packet sent 2x). File format: repeated [u32 delay_us LE][u16 len LE][payload]. */
static void replay_main(void *arg) {
    agent_t *a = (agent_t *)arg;
    FILE *f = fopen(a->replay_file, "rb");
    if (!f) { BSDR_ERROR("bsdr.agent", "replay: cannot open %s", a->replay_file); return; }
    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, BSDR_REMOTE_DESKTOP_PORT, a->remote_ip, BSDR_REMOTE_DESKTOP_PORT)) {
        BSDR_ERROR("bsdr.agent", "replay: udp open 45002 -> %s failed", a->remote_ip);
        fclose(f); return;
    }
    BSDR_INFO("bsdr.agent", "LAN REPLAY: streaming %s -> %s:%d (no DTLS)",
              a->replay_file, a->remote_ip, BSDR_REMOTE_DESKTOP_PORT);
    uint8_t buf[2048];
    long sent = 0;
    while (!a->stop) {
        uint8_t hdr[6];
        if (fread(hdr, 1, 6, f) != 6) { rewind(f); continue; }   /* loop the capture */
        uint32_t delay = hdr[0] | hdr[1]<<8 | hdr[2]<<16 | (uint32_t)hdr[3]<<24;
        uint16_t len = hdr[4] | hdr[5]<<8;
        if (len > sizeof(buf) || fread(buf, 1, len, f) != len) { rewind(f); continue; }
        if (delay > 0 && delay < 500000) bsdr_sleep_ms(delay / 1000);
        bsdr_udp_send(&udp, buf, len);
        if ((++sent % 2000) == 0)
            BSDR_INFO("bsdr.agent", "replay: %ld frames sent", sent);
    }
    bsdr_udp_close(&udp);
    fclose(f);
    BSDR_INFO("bsdr.agent", "replay stopped (%ld frames)", sent);
}

/* ---- LAN live video generator (BigSoup wire format, reversed from BigSoup.dll
 * FUN_180099050). Each H.264 NAL (Annex-B, SPS/PPS/IDR/P; SEI+AUD dropped) is
 * fragmented to <=1372B; each packet = [fragment][16B trailer], then bytes[1..]
 * of the fragment are XOR'd with 0x14 (byte0=NAL hdr + trailer stay plaintext),
 * sent twice on 45002. Trailer = [u32 sessid][u16 W][u16 H][u64 frame/frag/ts]. */
#ifdef BSDR_HAVE_CAPTURE
#define LAN_FRAG_MAX 0x55c          /* 1372 bytes payload per packet */
#define LAN_TRAILER  16

/* LAN video packet redundancy: the official host sends each fragment twice (UDP, no retransmit).
 * On a bandwidth-constrained WiFi link that doubling can itself saturate the link and cause the very
 * loss it's meant to hide; --lan-1x drops it to 1x to halve the uplink. --sendmmsg batches a whole
 * NAL's fragments into one syscall. Both set once at startup. */
static int g_lan_video_reps = 2;
static int g_lan_sendmmsg = 0;

/* Build one LAN wire packet for fragment `fi` into pkt[] ([fragment][16B trailer], XOR-0x14 on
 * bytes[1..flen)). Returns the packet length. */
static size_t lan_build_frag(uint8_t *pkt, const uint8_t *nal, size_t nlen, uint16_t fi, uint16_t total,
                             uint32_t sessid, uint16_t w, uint16_t h, uint32_t frame_num, uint8_t ts_delta) {
    size_t off  = (size_t)fi * LAN_FRAG_MAX;
    size_t flen = nlen - off;
    if (flen > LAN_FRAG_MAX) flen = LAN_FRAG_MAX;
    memcpy(pkt, nal + off, flen);
    uint8_t *tr = pkt + flen;                      /* 16-byte plaintext trailer */
    tr[0]=sessid; tr[1]=sessid>>8; tr[2]=sessid>>16; tr[3]=sessid>>24;
    tr[4]=w; tr[5]=w>>8;  tr[6]=h; tr[7]=h>>8;
    uint64_t comp = ((((uint64_t)frame_num<<16 | fi)<<16 | total)<<8) | ts_delta;
    for (int b=0;b<8;b++) tr[8+b] = (uint8_t)(comp>>(8*b));
    for (size_t i=1;i<flen;i++) pkt[i] ^= 0x14;     /* skip byte0 (NAL hdr) + trailer */
    return flen + LAN_TRAILER;
}

static void lan_send_nal(bsdr_udp *udp, const uint8_t *nal, size_t nlen,
                         uint32_t sessid, uint16_t w, uint16_t h,
                         uint32_t frame_num, uint8_t ts_delta) {
    uint16_t total = (uint16_t)((nlen + LAN_FRAG_MAX - 1) / LAN_FRAG_MAX);
    if (total == 0) total = 1;
    int reps = g_lan_video_reps < 1 ? 1 : g_lan_video_reps;
#ifndef _WIN32
    if (g_lan_sendmmsg) {   /* build all fragments contiguously, then send every rep in one syscall */
        static __thread uint8_t *bbuf = NULL; static __thread size_t bcap = 0;
        static __thread struct iovec *iov = NULL; static __thread size_t icap = 0;
        size_t need = (size_t)total * (LAN_FRAG_MAX + LAN_TRAILER), niov = (size_t)total * reps;
        if (need > bcap) { uint8_t *nb = realloc(bbuf, need); if (nb) { bbuf = nb; bcap = need; } }
        if (niov > icap) { struct iovec *ni = realloc(iov, niov*sizeof(*iov)); if (ni) { iov = ni; icap = niov; } }
        if (bbuf && iov && bcap >= need && icap >= niov) {
            size_t bo = 0; int iv = 0;
            for (uint16_t fi = 0; fi < total; fi++) {
                uint8_t *pkt = bbuf + bo;
                size_t plen = lan_build_frag(pkt, nal, nlen, fi, total, sessid, w, h, frame_num, ts_delta);
                for (int r = 0; r < reps; r++) { iov[iv].iov_base = pkt; iov[iv].iov_len = plen; iv++; }
                bo += plen;
            }
            bsdr_udp_send_batch(udp, iov, iv);
            return;
        }
        /* allocation failed -> fall through to the per-fragment path */
    }
#endif
    uint8_t pkt[LAN_FRAG_MAX + LAN_TRAILER];
    for (uint16_t fi = 0; fi < total; fi++) {
        size_t plen = lan_build_frag(pkt, nal, nlen, fi, total, sessid, w, h, frame_num, ts_delta);
        for (int r = 0; r < reps; r++)
            bsdr_udp_send(udp, pkt, plen);
    }
}

/* LAN desktop-audio out (BigSoup wire format, FUN_180080b20): Opus 48k stereo,
 * 10ms/480-sample frames -> packet [Opus payload][8B trailer: u32 ssrc | u32 seq],
 * XOR-0x14 over the WHOLE Opus payload (byte0 included; trailer plaintext), sent on
 * the SAME 45002 socket as video (the Quest demuxes by content). */
#ifdef BSDR_HAVE_AUDIO
struct lan_audio_ctx { agent_t *a; bsdr_udp *udp; bsdr_audio_devices *dev; volatile int stop; };

/* desktop audio OUT (PC -> Quest): Opus 48k stereo. */
static void lan_audio_main(void *arg) {
    struct lan_audio_ctx *ctx = (struct lan_audio_ctx *)arg;
    bsdr_pa *cap = bsdr_pa_record_open(ctx->dev->monitor_source, BSDR_AUDIO_CHANNELS);
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(48000, BSDR_AUDIO_CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    if (!cap || !enc || err != 0) {
        BSDR_WARN("bsdr.agent", "LAN audio-out: init failed (err=%d)", err);
        if (cap) bsdr_pa_close(cap);
        if (enc) opus_encoder_destroy(enc);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BSDR_AUDIO_DESKTOP_BPS));
    uint32_t ssrc = 0; bsdr_random_bytes(&ssrc, sizeof ssrc); if (!ssrc) ssrc = BSDR_AUDIO_SSRC;
    uint32_t seq = 0;   /* fresh SSRC each session so a restart is a new RTP source, not a rollback */
    int16_t pcm[480 * BSDR_AUDIO_CHANNELS];
    uint8_t pkt[1500];
    BSDR_INFO("bsdr.agent", "LAN audio: desktop -> Quest:%d (Opus 48k stereo, XOR-0x14)",
              BSDR_REMOTE_AUDIO_PORT);
    while (!ctx->stop && !ctx->a->stop) {
        if (bsdr_pa_read(cap, pcm, 480) != 480) continue;
        int n = opus_encode(enc, pcm, 480, pkt, (int)sizeof(pkt) - 8);
        if (n <= 2) continue;
        uint8_t *tr = pkt + n;
        tr[0]=ssrc; tr[1]=ssrc>>8; tr[2]=ssrc>>16; tr[3]=ssrc>>24;
        tr[4]=seq;  tr[5]=seq>>8;  tr[6]=seq>>16;  tr[7]=seq>>24;
        for (int i = 0; i < n; i++) pkt[i] ^= 0x14;
        bsdr_udp_send(ctx->udp, pkt, (size_t)n + 8);
        seq++;
    }
    opus_encoder_destroy(enc); bsdr_pa_close(cap);
    BSDR_INFO("bsdr.agent", "LAN audio-out stopped (%u pkts)", seq);
}

#ifdef BSDR_HAVE_CAPTURE
/* file audio OUT (video file's own track -> Quest): decode+resample -> Opus, same LAN wire format
 * as lan_audio_main (Opus + 8-byte trailer, XOR-0x14). Volume/pause/seek follow the media bar. */
static void lan_file_audio_main(void *arg) {
    struct lan_audio_ctx *ctx = (struct lan_audio_ctx *)arg;
    agent_t *a = ctx->a;
    int pl_is = bsdr_path_is_playlist(a->video_file);   /* .txt playlist: play each once, advance at EOF */
    int pl_idx = 0;
    char curpath[512];
    if (bsdr_playlist_entry(a->video_file, pl_idx, curpath, sizeof curpath) == 0) return;
    bsdr_fileaudio *fa = bsdr_fileaudio_open(curpath, 48000, BSDR_AUDIO_CHANNELS, pl_is ? false : true);
    if (!fa) { BSDR_INFO("bsdr.agent", "LAN file audio: %s has no audio track", curpath); return; }
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(48000, BSDR_AUDIO_CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != 0) { BSDR_WARN("bsdr.agent", "LAN file audio: opus init failed (%d)", err);
                            bsdr_fileaudio_close(fa); return; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BSDR_AUDIO_DESKTOP_BPS));
    uint32_t ssrc = 0; bsdr_random_bytes(&ssrc, sizeof ssrc); if (!ssrc) ssrc = BSDR_AUDIO_SSRC;
    uint32_t seq = 0;   /* fresh SSRC each session so a restart is a new RTP source, not a rollback */
    unsigned my_seek = a->app->file_seek_gen;
    int16_t pcm[480 * BSDR_AUDIO_CHANNELS];
    uint8_t pkt[1500];
    BSDR_INFO("bsdr.agent", "LAN file audio: %s -> Quest:%d (Opus 48k %dch, XOR-0x14)",
              curpath, BSDR_REMOTE_AUDIO_PORT, BSDR_AUDIO_CHANNELS);
    while (!ctx->stop && !a->stop) {
        bsdr_fileaudio_set_paused(fa, a->app->file_paused);
        if (a->app->file_seek_gen != my_seek) { bsdr_fileaudio_seek(fa, a->app->file_seek_frac); my_seek = a->app->file_seek_gen; }
        int got = bsdr_fileaudio_read(fa, pcm, 480);
        if (got < 0 && pl_is) {   /* clip ended -> next playlist entry (its own audio; may have none) */
            bsdr_fileaudio_close(fa); fa = NULL;
            for (int tries = 0; tries < 64 && !fa; tries++) {
                if (bsdr_playlist_entry(a->video_file, ++pl_idx, curpath, sizeof curpath) == 0) break;
                fa = bsdr_fileaudio_open(curpath, 48000, BSDR_AUDIO_CHANNELS, false);
            }
            if (!fa) { bsdr_sleep_ms(20); if (bsdr_playlist_entry(a->video_file, pl_idx, curpath, sizeof curpath)) continue; break; }
            continue;
        }
        if (got <= 0) { if (got < 0) bsdr_sleep_ms(5); continue; }   /* <0 EOF(non-loop); 0 paused/underrun */
        int vol = a->app->file_volume;
        if (vol != 100) for (int i = 0; i < 480 * BSDR_AUDIO_CHANNELS; i++) pcm[i] = (int16_t)(pcm[i] * vol / 100);
        int n = opus_encode(enc, pcm, 480, pkt, (int)sizeof(pkt) - 8);
        if (n <= 2) continue;
        uint8_t *tr = pkt + n;
        tr[0]=ssrc; tr[1]=ssrc>>8; tr[2]=ssrc>>16; tr[3]=ssrc>>24;
        tr[4]=seq;  tr[5]=seq>>8;  tr[6]=seq>>16;  tr[7]=seq>>24;
        for (int i = 0; i < n; i++) pkt[i] ^= 0x14;
        bsdr_udp_send(ctx->udp, pkt, (size_t)n + 8);
        seq++;
    }
    opus_encoder_destroy(enc); bsdr_fileaudio_close(fa);
    BSDR_INFO("bsdr.agent", "LAN file audio-out stopped (%u pkts)", seq);
}
#endif /* BSDR_HAVE_CAPTURE */

/* NOTE: the remote-desktop protocol has NO mic-upload channel at all. Confirmed from a real
 * Quest<->host capture (full.pcapng): the headset's entire upstream is fixed-size 88-byte
 * controller/input telemetry on 45004 (~75/s, headset refresh rate — never variable-bitrate
 * Opus), video keepalives on 45002, and discovery; 45003 carries ZERO Quest->PC traffic. The
 * owner's voice only ever goes to the room's mediasoup cloud, as plain Opus RTP. To expose it
 * locally we sniff that Quest->cloud stream off the LAN — see src/micsniff.c (--sniff-mic). */
#endif

/* LAN input channel (Quest -> PC), reversed from BigSoup.dll input dispatcher
 * (0x18008ee40): the data channel on 45004 IS DTLS (Quest=client, PC=server). After
 * the handshake the host sends a 5-byte hello, then receives raw opcode bytes over
 * DTLS (byte0=opcode; NO XOR — DTLS encrypts). We decode + inject via uinput. */
struct lan_input_ctx { agent_t *a; int sw, sh; volatile int stop; };
static void lan_input_main(void *arg) {
    struct lan_input_ctx *ctx = (struct lan_input_ctx *)arg;
    agent_t *a = ctx->a;
    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, BSDR_REMOTE_DATA_PORT, a->remote_ip, BSDR_REMOTE_DATA_PORT)) {
        BSDR_WARN("bsdr.agent", "LAN input: udp 45004 open failed"); return;
    }
    bsdr_dtls *dtls = bsdr_dtls_new(&udp, BSDR_DTLS_SERVER);   /* Quest is the client */
    if (!dtls) { bsdr_udp_close(&udp); return; }
    BSDR_INFO("bsdr.agent", "LAN input: DTLS server on 45004, awaiting Quest...");
    if (!bsdr_dtls_handshake(dtls, 30000, &a->stop)) {
        BSDR_WARN("bsdr.agent", "LAN input: DTLS handshake timeout (no Quest on 45004)");
        bsdr_dtls_free(dtls); bsdr_udp_close(&udp); return;
    }
    static const uint8_t hello[5] = { 0x10, 0, 0, 0, 0 };       /* host data-channel hello */
    bsdr_dtls_send(dtls, hello, sizeof(hello));
    bsdr_injector *inj = bsdr_injector_create(ctx->sw, ctx->sh);
    BSDR_INFO("bsdr.agent", "LAN input: DTLS connected; injecting Quest mouse/keyboard");
    uint8_t buf[2048];
    bsdr_input_event evs[32];
    long n_ev = 0;
    /* Diagnostic only: log the shape of any 45004 message the input decoder doesn't consume.
     * These are NOT mic audio — full.pcapng proved 45004 upstream is fixed-size controller
     * telemetry at the headset refresh rate; the mic never comes here (see the sniffer). */
    long size_hist[16] = {0}; uint8_t op_seen[256] = {0};
    /* Voice-command balloon interaction (all consumed, not injected to the desktop):
     *   - drag the main balloon to move it; a click (no drag) toggles listening:
     *       IDLE -> start; LISTENING -> stop (go to the Send/Cancel prompt).
     *   - the Send / Cancel buttons resolve a stopped capture.
     *   - the stop balloon (shown while a command runs) aborts it.
     *   - the log handle toggles the history panel.
     * We track the last absolute pointer position since clicks carry no coords. */
    double last_x = 0.5, last_y = 0.5, down_x = 0, down_y = 0;
    int balloon_drag = 0, balloon_moved = 0, swallow_up = 0;
    while (!ctx->stop && !a->stop) {
        int n = bsdr_dtls_recv(dtls, buf, sizeof(buf), 100);
        if (n < 0) break;
        if (n == 0) continue;
        size_t ne = bsdr_decode_binary(buf, (size_t)n, evs, 32);
        for (size_t i = 0; i < ne; i++) {
            bsdr_input_event *e = &evs[i];
            struct bsdr_overlay *ovl = a->overlay;
            int balloon = ovl && bsdr_overlay_balloon_on(ovl);
            if (e->kind == BSDR_EV_MOVE_ABS) {
                last_x = e->u.move_abs.x; last_y = e->u.move_abs.y;
                if (balloon && balloon_drag) {              /* drag: move the balloon, swallow */
                    bsdr_overlay_set_balloon_pos(ovl, last_x, last_y);
                    double ddx = last_x - down_x, ddy = last_y - down_y;
                    if (ddx*ddx + ddy*ddy > 0.0004) balloon_moved = 1;   /* ~2% travel = a drag */
                    continue;
                }
            } else if (a->file_mode && ovl && bsdr_overlay_visible(ovl) &&
                       e->kind == BSDR_EV_BUTTON && e->u.button.button == BSDR_BTN_LEFT) {
                /* Media bar (file streaming): a click on the bar drives playback and is swallowed;
                 * a click elsewhere falls through (injected) exactly as before. */
                if (e->u.button.down) {
                    double val = 0;
                    bsdr_overlay_action act = bsdr_overlay_hit(ovl, last_x, last_y, &val);
                    switch (act) {
                        case BSDR_OVL_PLAYPAUSE: a->app->file_paused = !a->app->file_paused; break;
                        case BSDR_OVL_SEEK:      a->app->file_seek_frac = val; a->app->file_seek_gen++; break;
                        case BSDR_OVL_VOL_DOWN:  { int v = a->app->file_volume - 10; a->app->file_volume = v < 0 ? 0 : v; } break;
                        case BSDR_OVL_VOL_UP:    { int v = val > 0 ? (int)(val * 100) : a->app->file_volume + 10;
                                                   a->app->file_volume = v < 0 ? 0 : v > 100 ? 100 : v; } break;
                        case BSDR_OVL_EXIT:      a->stop = 1; break;
                        default: break;   /* NONE / VOICE: not on the bar */
                    }
                    if (act != BSDR_OVL_NONE) { swallow_up = 1; continue; }
                } else if (swallow_up) { swallow_up = 0; continue; }
            } else if (balloon && e->kind == BSDR_EV_BUTTON && e->u.button.button == BSDR_BTN_LEFT) {
                if (e->u.button.down) {
                    int ch;
                    if (bsdr_overlay_stop_hit(ovl, last_x, last_y)) {       /* abort a running command */
                        if (a->voice) bsdr_voice_abort(a->voice);
                        swallow_up = 1; continue;
                    } else if ((ch = bsdr_overlay_confirm_hit(ovl, last_x, last_y)) != 0) {
                        if (a->voice) bsdr_voice_confirm(a->voice, ch == 1);   /* 1=Send 2=Cancel */
                        swallow_up = 1; continue;
                    } else if (bsdr_overlay_history_hit(ovl, last_x, last_y)) {
                        bsdr_overlay_toggle_history(ovl);
                        swallow_up = 1; continue;
                    } else if (bsdr_overlay_balloon_hit(ovl, last_x, last_y)) {
                        balloon_drag = 1; balloon_moved = 0; down_x = last_x; down_y = last_y;
                        continue;
                    }
                } else {                                    /* button up */
                    if (swallow_up) { swallow_up = 0; continue; }
                    if (balloon_drag) {
                        balloon_drag = 0;
                        if (!balloon_moved && a->voice) {   /* a click (not a drag) */
                            int st = bsdr_voice_state_get(a->voice);
                            if (st == BSDR_VST_LISTENING) bsdr_voice_stop_capture(a->voice);
                            else if (st == BSDR_VST_IDLE)  bsdr_voice_trigger(a->voice, BSDR_VOICE_COMMAND);
                        }
                        continue;
                    }
                }
            }
            bsdr_injector_handle(inj, e); n_ev++;
        }
        if (ne == 0) {   /* not an input message — controller telemetry, not mic */
            if (!op_seen[buf[0]]) {   /* first sighting of this leading byte */
                op_seen[buf[0]] = 1;
                BSDR_DEBUG("bsdr.agent", "LAN 45004 non-input msg: byte0=0x%02x len=%d",
                          buf[0], n);
            }
            size_hist[n < 1500 ? (n / 100) : 15]++;
        }
    }
    BSDR_INFO("bsdr.agent", "LAN 45004 non-input size histogram (per 100B): "
              "0-99=%ld 100-199=%ld 200-299=%ld 300+=%ld",
              size_hist[0], size_hist[1], size_hist[2],
              size_hist[3]+size_hist[4]+size_hist[5]+size_hist[6]);
    if (inj) bsdr_injector_destroy(inj);
    bsdr_dtls_free(dtls); bsdr_udp_close(&udp);
    BSDR_INFO("bsdr.agent", "LAN input stopped (%ld events injected)", n_ev);
}

/* True if 2D->3D is enabled in the app config right now. */
static int threed_on(agent_t *a) {
    if (!a->app) return 0;
    int m = 0; bsdr_app_get_threed(a->app, &m, NULL, NULL, NULL, NULL, NULL, NULL, 0);
    return m != BSDR_3D_OFF;
}
/* Fill the capture config's 2D->3D fields from the app (call before each bsdr_capture_open). On
 * Android the raw frame lives in the Kotlin MediaProjection->MediaCodec path (never in C), so we
 * publish the config for the GL pipeline instead and leave the C capture's threed off. */
static void threed_cfg(agent_t *a, bsdr_capture_config *cfg) {
    if (!a->app) return;
    int m = 0, deep = 0, conv = 0, swap = 0, full = 0, tier = 0; char ai[256] = "";
    bsdr_app_get_threed(a->app, &m, &deep, &conv, &swap, &full, &tier, ai, sizeof ai);
#if defined(BSDR_PLATFORM_ANDROID)
    bsdr_android_publish_threed(m, deep, conv, swap, full, tier);   /* GL applies SBS; tier drives NNAPI depth */
    cfg->threed_mode = 0;
#else
    cfg->threed_mode = m; cfg->threed_deepness = deep; cfg->threed_convergence = conv;
    cfg->threed_swap = swap; cfg->threed_full = full; cfg->threed_tier = tier;
    snprintf(cfg->threed_ai_cmd, sizeof cfg->threed_ai_cmd, "%s", ai);
#endif
}

/* Fill the capture config's webcam fields from the app source (call before each non-file
 * bsdr_capture_open). "webcam" -> single camera in cfg->webcam; "webcam3d" -> left+right for the
 * stereo SBS compositor. Devices are copied into the agent's stable storage so the pointers survive
 * the capture's lifetime. Returns 1 if a webcam source is active (so the caller skips screen grab). */
static int webcam_cfg(agent_t *a, bsdr_capture_config *cfg) {
    cfg->webcam = cfg->webcam_right = NULL;
    if (!a->app) return 0;
    char mode[16] = "", left[256] = "";
    bsdr_app_get_source(a->app, mode, sizeof mode, left, sizeof left);
    int stereo = strcmp(mode, "webcam3d") == 0;
    if (strcmp(mode, "webcam") != 0 && !stereo) return 0;
    snprintf(a->cam_left, sizeof a->cam_left, "%s", left);
    cfg->webcam = a->cam_left[0] ? a->cam_left : NULL;
    if (stereo) {
        bsdr_app_get_source_right(a->app, a->cam_right, sizeof a->cam_right);
        cfg->webcam_right = a->cam_right[0] ? a->cam_right : NULL;
    }
    return cfg->webcam != NULL;
}

/* Open/close the face-swap engine to match the app state, (re)loading the source image on enable.
 * Models live in <model cache>/faceswap (det_10g/w600k_r50/inswapper_128, user-supplied). Closes
 * `cur` first; returns the new engine (or NULL when off/unavailable). */
static bsdr_faceswap *faceswap_reconcile(agent_t *a, bsdr_faceswap *cur) {
    if (cur) bsdr_faceswap_close(cur);
    if (!a->app) return NULL;
#if defined(BSDR_PLATFORM_ANDROID)
    return NULL;   /* Android applies face swap in the Kotlin GL pipeline, not this C capture path */
#else
    bool on=false; int tier=0; char src[512]="";
    bsdr_app_get_faceswap(a->app, &on, &tier, src, sizeof src);
    if (!on) { bsdr_app_set_faceswap_status(a->app, "off"); return NULL; }
    char dir[768], fsdir[900];
    bsdr_model_dir(dir, sizeof dir);
    snprintf(fsdir, sizeof fsdir, "%s/faceswap", dir);
    bsdr_faceswap *fs = bsdr_faceswap_open(fsdir, tier >= 2);
    if (!fs) { bsdr_app_set_faceswap_status(a->app, "models missing — put det_10g/w600k_r50/inswapper_128 in the faceswap dir"); return NULL; }
    if (!src[0]) { bsdr_app_set_faceswap_status(a->app, "set a source image"); return fs; }
    uint8_t *rgb=NULL; int w=0,h=0;
    if (bsdr_capture_decode_image_rgb(src, &rgb, &w, &h) != 0) { bsdr_app_set_faceswap_status(a->app, "cannot read source image"); return fs; }
    int r = bsdr_faceswap_set_source_rgb(fs, rgb, w, h);
    free(rgb);
    bsdr_app_set_faceswap_status(a->app, r == 0 ? bsdr_faceswap_status(fs) : "no face found in source image");
    return fs;
#endif
}

static void lan_live_main(agent_t *a) {
    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, BSDR_REMOTE_DESKTOP_PORT, a->remote_ip, BSDR_REMOTE_DESKTOP_PORT)) {
        BSDR_ERROR("bsdr.agent", "LAN: udp 45002 -> %s failed", a->remote_ip); return;
    }
    bsdr_capture_config cfg = {0};
    cfg.fps = a->fps > 0 ? a->fps : 30;
    cfg.bitrate = a->bitrate > 0 ? a->bitrate : 8000000;
    int user_cpu = a->app && a->app->cpu_only;   /* the operator's --cpu (best-quality software x264) */
    cfg.cpu_only = user_cpu;
    cfg.use_vaapi = a->app && a->app->use_vaapi;
    cfg.use_kmsgrab = a->app && a->app->use_kmsgrab;
    /* 2D->3D SBS runs on the CPU NV12 frame, so it needs the CPU-scale path (no VAAPI/CUDA scale
     * and no zero-copy kmsgrab). Force CPU *scale* whenever 3D is enabled — but keep NVENC for the
     * ENCODE (it accepts a software NV12 frame and, unlike x264, the Quest decodes its stream without
     * freezing on the SBS content). Only the operator's own --cpu forces the libx264 encoder. */
    if (threed_on(a)) { cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
    /* --cpu means the FULL software path: CPU scale AND libx264 encode. Without --cpu the desktop
     * auto-picks NVENC (2-pass, p7) even when 3D forced the CPU-scale path above. */
    if (user_cpu) cfg.encoder = "libx264";
    threed_cfg(a, &cfg);   /* capture builds the SBS transform from these cfg fields */
    /* face swap runs on the CPU NV12 frame (like 3D) -> force the CPU scale path when it's on. */
    bsdr_faceswap *fs_engine = faceswap_reconcile(a, NULL);
    unsigned my_fs_gen = a->app ? a->app->faceswap_gen : 0;
    if (fs_engine) { cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
    int rx=0,ry=0,rw=0,rh=0;                           /* capture region; 0s = whole desktop */
    int qw=0,qh=0,qbr=0;                                /* live quality (headset PUT /device) */
    int w=0,h=0; const char *enc="h264";
    bsdr_capture *cap = NULL;
    int filemode = (a->video_file != NULL);
    int pl_is = filemode && bsdr_path_is_playlist(a->video_file);   /* .txt = playlist */
    int pl_idx = 0;
    char curpath[512] = "";
    if (filemode) {                                    /* --file / web-UI file source (or .txt playlist) */
        /* Decode+re-encode the file through the capture pipeline so the in-VR media bar composites
         * onto the video (and, via the coupled cloud feed, onto the room stream too). A .txt source
         * is a playlist: play each entry once (loop=0) and advance at EOF; a single file self-loops. */
        int pl_n = bsdr_playlist_entry(a->video_file, pl_idx, curpath, sizeof curpath);
        if (pl_n == 0) { BSDR_ERROR("bsdr.agent", "LAN: empty/unreadable source %s", a->video_file);
                         bsdr_udp_close(&udp); return; }
        if (a->app) bsdr_app_get_quality(a->app, &qw, &qh, &qbr);
        if (qbr > 0) cfg.bitrate = qbr;
        cfg.out_width = qw > 0 ? qw : 0; cfg.out_height = qh > 0 ? qh : 0;
        cfg.input_file = curpath; cfg.loop = pl_is ? 0 : 1;
        /* Default to libx264 (better quality than NVENC at the low bitrates the Quest asks for);
         * --file-gpu opts into NVENC (auto: nvenc then x264 fallback). Both composite the bar via
         * the CPU-scale path (capture forces CPU scale in file mode). */
        cfg.encoder = a->file_gpu ? NULL : "libx264";
        cap = bsdr_capture_open(&cfg);
        if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: cannot open video file %s", curpath);
                    bsdr_udp_close(&udp); return; }
        bsdr_capture_info(cap, &w, &h, &enc);
        a->file_mode = 1; a->app->file_paused = 0;
        if (a->app->file_volume <= 0) a->app->file_volume = 100;
        if (a->overlay) {
            bsdr_capture_set_overlay(cap, a->overlay);
            bsdr_overlay_set_visible(a->overlay, true);
            bsdr_overlay_set_playing(a->overlay, true);
            bsdr_overlay_set_volume(a->overlay, a->app->file_volume);
        }
        BSDR_INFO("bsdr.agent", "LAN LIVE: %s%s %dx%d via %s (media bar) -> %s:%d (XOR-0x14)",
                  pl_is ? "playlist entry " : "file ", curpath, w, h, enc?enc:"?",
                  a->remote_ip, BSDR_REMOTE_DESKTOP_PORT);
    } else {
        int is_cam = webcam_cfg(a, &cfg);   /* webcam source -> sets cfg.webcam[_right]; else screen grab */
        if (a->app) bsdr_app_get_region(a->app, &rx, &ry, &rw, &rh);
        cfg.x = rx; cfg.y = ry; cfg.width = rw; cfg.height = rh;
        if (a->app) bsdr_app_get_quality(a->app, &qw, &qh, &qbr);
        if (qbr > 0) cfg.bitrate = qbr;
        cfg.out_width = qw > 0 ? qw : 0; cfg.out_height = qh > 0 ? qh : 0;
        cap = bsdr_capture_open(&cfg);
        if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: %s capture/encode open failed",
                    is_cam ? "webcam" : "desktop");
                    bsdr_udp_close(&udp); return; }
        if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);   /* voice-command balloon */
        bsdr_capture_info(cap, &w, &h, &enc);
        BSDR_INFO("bsdr.agent", "LAN LIVE: %s %dx%d via %s -> %s:%d (XOR-0x14, no DTLS)",
                  is_cam ? "webcam" : "desktop", w, h, enc?enc:"?", a->remote_ip, BSDR_REMOTE_DESKTOP_PORT);
    }
    uint16_t tw = (uint16_t)((w+15)&~15), th = (uint16_t)((h+15)&~15);  /* MB-aligned, like the host */
    /* Per-stream id, RANDOM each session. It must change across bsdrX restarts: the Quest keys its
     * frame-reassembly ("BigFrame") queue on (sessid, frame_num), and reusing a constant sessid while
     * frame_num resets to 0 on restart makes frame numbers roll backwards under the same session ->
     * the Quest's queue chokes and the headset crashes. A fresh sessid = a clean new stream. */
    uint32_t sessid = 0;
    bsdr_random_bytes(&sessid, sizeof sessid);
    if (sessid == 0) sessid = 0x2a8a3c48;
    uint32_t frame_num = 0; long pkts = 0;
    uint64_t prev = bsdr_now_ms();
#ifdef BSDR_HAVE_AUDIO
    /* Desktop audio needs the PulseAudio monitor (adev); a video file streams its own track and
     * needs no virtual devices. Both go out on 45003 (separate from video 45002). */
    bsdr_audio_devices adev;
    bool adev_ok = a->audio && !filemode && bsdr_audio_devices_create(&adev);
    if (a->audio && !filemode && !adev_ok) BSDR_WARN("bsdr.agent", "LAN: virtual audio devices failed; audio off");
    bsdr_udp audio_udp;
    bool audio_ok = a->audio && (filemode || adev_ok) &&
        bsdr_udp_open(&audio_udp, BSDR_REMOTE_AUDIO_PORT, a->remote_ip, BSDR_REMOTE_AUDIO_PORT);
    if (a->audio && (filemode || adev_ok) && !audio_ok) BSDR_WARN("bsdr.agent", "LAN: audio udp 45003 open failed; audio off");
    struct lan_audio_ctx actx = { a, &audio_udp, adev_ok ? &adev : NULL, 0 };
    bsdr_thread *athr = audio_ok ? bsdr_thread_start(filemode ? lan_file_audio_main : lan_audio_main, &actx)
                                 : NULL;   /* -> Quest:45003 */
#endif
    struct lan_input_ctx ictx = { a, w, h, 0 };       /* Quest mouse/keyboard -> uinput */
    bsdr_thread *ithr = bsdr_thread_start(lan_input_main, &ictx);
    unsigned my_seek_gen = a->app->file_seek_gen;
    unsigned my_threed_gen = a->app->threed_gen;
    if (cap) bsdr_capture_set_faceswap(cap, fs_engine);
    while (!a->stop) {
        if (cap) bsdr_capture_set_faceswap(cap, fs_engine);   /* re-attach across any reopen below */
        /* Face swap toggled/retuned from the web UI: reconcile the engine (reload model+source) and,
         * because it forces the CPU encode path, reopen the capture. */
        if (a->app && a->app->faceswap_gen != my_fs_gen) {
            my_fs_gen = a->app->faceswap_gen;
            fs_engine = faceswap_reconcile(a, fs_engine);
            int fs_on = fs_engine != NULL, td = threed_on(a);
            cfg.cpu_only = fs_on || td || user_cpu;
            if (fs_on || td) { cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
            else { cfg.use_vaapi = a->app->use_vaapi; cfg.use_kmsgrab = a->app->use_kmsgrab; }
            if (filemode) cfg.encoder = a->file_gpu ? NULL : "libx264";
            else if (user_cpu) cfg.encoder = "libx264"; else cfg.encoder = NULL;
            threed_cfg(a, &cfg); if (!filemode) webcam_cfg(a, &cfg);
            bsdr_capture_close(cap); cap = bsdr_capture_open(&cfg);
            if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: reopen for faceswap change failed"); break; }
            if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
            bsdr_capture_set_faceswap(cap, fs_engine);
            bsdr_capture_info(cap, &w, &h, &enc);
            tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
        }
        /* Media-bar controls (file mode): apply pause + edge-triggered seek to the capture. */
        if (filemode) {
            bsdr_capture_set_paused(cap, a->app->file_paused);
            if (a->app->file_seek_gen != my_seek_gen) {
                bsdr_capture_seek(cap, a->app->file_seek_frac);
                my_seek_gen = a->app->file_seek_gen;
            }
        }
        /* 2D->3D toggled/retuned from the web UI: reopen the capture so the CPU-scale path and a
         * fresh transform take effect (works for both desktop and file sources). */
        if (a->app && a->app->threed_gen != my_threed_gen) {
            my_threed_gen = a->app->threed_gen;
            int on = threed_on(a);
            cfg.cpu_only = on || user_cpu;
            if (on) { cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
            else { cfg.use_vaapi = a->app->use_vaapi; cfg.use_kmsgrab = a->app->use_kmsgrab; }
            /* 3D keeps NVENC (Quest decodes it cleanly); only --cpu or file-default forces x264. */
            if (filemode) cfg.encoder = a->file_gpu ? NULL : "libx264";
            else if (user_cpu) cfg.encoder = "libx264"; else cfg.encoder = NULL;
            threed_cfg(a, &cfg);
            if (!filemode) webcam_cfg(a, &cfg);   /* keep the webcam source across the 3D reopen */
            bsdr_capture_close(cap);
            cap = bsdr_capture_open(&cfg);
            if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: reopen for 3D change failed"); break; }
            if (filemode && a->overlay) {
                bsdr_capture_set_overlay(cap, a->overlay);
                bsdr_overlay_set_visible(a->overlay, true);
            } else if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
            bsdr_capture_info(cap, &w, &h, &enc);
            tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
            BSDR_INFO("bsdr.agent", "LAN 3D reconfig: mode=%s %dx%d (fresh keyframe)",
                      on ? "on" : "off", w, h);
        }
        /* live re-config (desktop capture only): web UI window switch, or headset
         * bitrate/resolution (PUT /device). A file source has fixed dimensions. */
        if (!filemode && a->app) {
            int nx,ny,nw,nh; bsdr_app_get_region(a->app, &nx,&ny,&nw,&nh);
            int nqw,nqh,nqbr; bsdr_app_get_quality(a->app, &nqw,&nqh,&nqbr);
            if (nx!=rx || ny!=ry || nw!=rw || nh!=rh ||
                nqw!=qw || nqh!=qh || (nqbr>0 && nqbr!=qbr)) {
                rx=nx; ry=ny; rw=nw; rh=nh; qw=nqw; qh=nqh; if (nqbr>0) qbr=nqbr;
                bsdr_capture_close(cap);
                cfg.x=rx; cfg.y=ry; cfg.width=rw; cfg.height=rh;
                cfg.out_width = qw>0?qw:0; cfg.out_height = qh>0?qh:0;
                if (qbr>0) cfg.bitrate = qbr;
                webcam_cfg(a, &cfg);   /* keep the webcam source across the live reconfig reopen */
                cap = bsdr_capture_open(&cfg);
                if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: re-open for reconfig failed"); break; }
                if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
                bsdr_capture_info(cap,&w,&h,&enc);
                tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
                BSDR_INFO("bsdr.agent", "LAN live reconfig: %s %dx%d @ %d bps (fresh keyframe)",
                          (rw&&rh)?"window":"desktop", w, h, cfg.bitrate);
            }
        }
        const uint8_t *au; size_t len; uint32_t rtp_ts;
        int r = bsdr_capture_frame(cap, &au, &len, &rtp_ts);
        if (r < 0 && pl_is) {
            /* End of this clip -> advance to the next playlist entry (wraps). Skip any that fail to
             * open so one bad path can't stall the show; give up only if none open. */
            bsdr_capture_close(cap); cap = NULL;
            for (int tries = 0; tries < 64; tries++) {
                char next[512];
                if (bsdr_playlist_entry(a->video_file, ++pl_idx, next, sizeof next) == 0) break;
                cfg.input_file = next;
                cap = bsdr_capture_open(&cfg);
                if (cap) {
                    if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
                    bsdr_capture_info(cap, &w, &h, &enc);
                    tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
                    BSDR_INFO("bsdr.agent", "LAN playlist -> %s %dx%d (fresh keyframe)", next, w, h);
                    break;
                }
                BSDR_WARN("bsdr.agent", "LAN playlist: skipping unopenable %s", next);
            }
            if (!cap) { BSDR_ERROR("bsdr.agent", "LAN playlist: no playable entries"); break; }
            continue;
        }
        if (r <= 0) { if (r < 0) break; bsdr_sleep_ms(2); continue; }
        /* Reflect playback state onto the bar (progress, play/pause icon, volume). */
        if (filemode && a->overlay) {
            int seekable = 0; double pos = bsdr_capture_position(cap, &seekable);
            bsdr_overlay_set_position(a->overlay, pos, seekable);
            bsdr_overlay_set_playing(a->overlay, !a->app->file_paused);
            bsdr_overlay_set_volume(a->overlay, a->app->file_volume);
        }
        /* COUPLED cloud (default): hand this same encoded access unit to the relay sender, so the
         * cloud reuses the single LAN encode (no second capture/encode). No-op when sharing is off
         * or in --video-decoupled mode (the relay self-captures then). */
        bsdr_app_feed_cloud_video(a->app, au, len, tw, th);
        uint64_t now = bsdr_now_ms();
        uint8_t ts_delta = (uint8_t)(now - prev); prev = now;
        /* split Annex-B access unit into NALs (strip 3/4-byte start codes) */
        const uint8_t *p = au, *end = au + len;
        const uint8_t *nal = NULL;
        while (p + 3 <= end) {
            int sc = (p[0]==0&&p[1]==0&&p[2]==1) ? 3 :
                     (p+4<=end&&p[0]==0&&p[1]==0&&p[2]==0&&p[3]==1) ? 4 : 0;
            if (sc) {
                if (nal) {                            /* flush previous NAL */
                    size_t nlen = (size_t)(p - nal);
                    uint8_t t = nal[0] & 0x1f;
                    if (nlen && t!=6 && t!=9 && t!=12)
                        { lan_send_nal(&udp, nal, nlen, sessid, tw, th, frame_num++, ts_delta); pkts++; }
                }
                p += sc; nal = p;
            } else p++;
        }
        if (nal && nal < end) {                       /* last NAL */
            size_t nlen = (size_t)(end - nal);
            uint8_t t = nal[0] & 0x1f;
            if (nlen && t!=6 && t!=9 && t!=12)
                { lan_send_nal(&udp, nal, nlen, sessid, tw, th, frame_num++, ts_delta); pkts++; }
        }
        if ((frame_num % 300) == 0)
            BSDR_DEBUG("bsdr.agent", "LAN live: %u NALs, %ld bursts sent", frame_num, pkts);
    }
#ifdef BSDR_HAVE_AUDIO
    actx.stop = 1;
    if (athr) bsdr_thread_join(athr);
    if (audio_ok) bsdr_udp_close(&audio_udp);
    if (adev_ok) bsdr_audio_devices_destroy(&adev);
#endif
    if (ithr) { ictx.stop = 1; bsdr_thread_join(ithr); }
    if (filemode && a->overlay) bsdr_overlay_set_visible(a->overlay, false);
    a->file_mode = 0;
    if (cap) bsdr_capture_close(cap);
    if (fs_engine) bsdr_faceswap_close(fs_engine);
    bsdr_udp_close(&udp);
    BSDR_INFO("bsdr.agent", "LAN live stopped (%u NALs sent)", frame_num);
}
#endif /* BSDR_HAVE_CAPTURE */

static void worker_main(void *arg) {
    agent_t *a = (agent_t *)arg;
    if (a->replay_file) { replay_main(a); return; }   /* LAN replay (diagnostic) */
#ifdef BSDR_HAVE_CAPTURE
    lan_live_main(a);   /* the real, validated LAN remote-desktop protocol */
#else
    BSDR_WARN("bsdr.agent", "this build has no capture support; nothing to stream");
#endif
}

static void teardown_session(agent_t *a) {
    bsdr_mutex_lock(a->lock);
    a->stop = 1;
    bsdr_thread *w = a->worker;
    a->worker = NULL;
    bsdr_mutex_unlock(a->lock);
    if (w) bsdr_thread_join(w);   /* the worker owns its own capture/udp/injector */
}

/* Privacy screen-blank: black out the PHYSICAL monitor via RandR brightness. Brightness is a gamma
 * ramp applied by the CRTC at scanout, so x11grab (which reads the framebuffer) still captures full
 * content for the Quest while the local screen shows black. Unlike DPMS it isn't woken by the
 * injected mouse/keyboard. Applied to every connected output; best-effort; no-op off X11. */
#if !defined(_WIN32) && !defined(__APPLE__)
static void screen_set_blank(int on) {
    if (!getenv("DISPLAY")) return;
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "for o in $(xrandr -q 2>/dev/null | grep ' connected' | cut -d' ' -f1); do "
        "xrandr --output \"$o\" --brightness %s 2>/dev/null; done",
        on ? "0" : "1");
    if (system(cmd) != 0) { /* best-effort */ }
}
#else
static void screen_set_blank(int on) { (void)on; }
#endif

/* start the session worker. lock held. */
static void spawn_worker_locked(agent_t *a) {
    if (a->app) {   /* web UI source picker: "file" -> path; "desktop"/"webcam"/"webcam3d" -> live capture */
        char mode[16] = "";
        bsdr_app_get_source(a->app, mode, sizeof(mode), a->src_path, sizeof(a->src_path));
        /* Only file mode sets video_file; clear it for every live source so a switch away from a
         * file (e.g. file -> webcam) actually leaves file mode on the next (re)spawn. */
        a->video_file = (strcmp(mode, "file") == 0 && a->src_path[0]) ? a->src_path : NULL;
    }
    a->stop = 0;
    a->worker = bsdr_thread_start(worker_main, a);
}

/* --- control callbacks (fire on the control thread; stay non-blocking) -----*/
static void cb_start(const bsdr_paired_device *dev, void *user) {
    agent_t *a = (agent_t *)user;
    BSDR_INFO("bsdr.agent", "device %s requested start; bringing up input link "
              "(DTLS data channel %s:%d)",
              dev->device_name, dev->remote_ip, BSDR_REMOTE_DATA_PORT);
    teardown_session(a);
    if (a->app) {
        bsdr_app_set_paired(a->app, true, dev->device_name, dev->remote_ip);
        bsdr_app_set_streaming(a->app, true);
    }
    bsdr_mutex_lock(a->lock);
    snprintf(a->remote_ip, sizeof(a->remote_ip), "%s", dev->remote_ip);
    spawn_worker_locked(a);
    bsdr_mutex_unlock(a->lock);
    /* NOTE: /start is the LAN session start, NOT the internet-sharing toggle — that comes via
     * PUT /device {isInternetSharing} (see cb_settings). Don't enable sharing here. */
}
static void cb_stop(const bsdr_paired_device *dev, void *user)   {
    agent_t *a = (agent_t *)user;
    BSDR_DEBUG("bsdr.agent", "headset %s requested STOP sharing", dev->device_name);
    if (a->app) { bsdr_app_set_streaming(a->app, false);
                  bsdr_app_set_internet_sharing(a->app, false); }  /* Quest disabled → stop relay */
    teardown_session(a);
}
static void cb_unpair(const bsdr_paired_device *dev, void *user) {
    agent_t *a = (agent_t *)user;
    BSDR_DEBUG("bsdr.agent", "headset %s requested UNPAIR", dev->device_name);
    if (a->app) { bsdr_app_set_paired(a->app, false, NULL, NULL);
                  bsdr_app_set_internet_sharing(a->app, false); }
    teardown_session(a);
}

/* The headset's PUT /device sends ONE of bitrate/fec/fps/resolution.
 * Units confirmed by disassembling BigSoup.dll (BigSoupSetDesktopStreaming*):
 *   - bitrate is plain bits-per-second; the DLL clamps to [500_000, 100_000_000].
 *   - resolution is the "streaming constraint" — a target height in px (480/720/1080).
 * The native side just stores these and flips a dirty flag (live reconfigure), which
 * is exactly what bsdr_transport_set_quality() does here — no connection teardown. */
static void cb_settings(const bsdr_paired_device *dev, void *user) {
    agent_t *a = (agent_t *)user;
    if (!a->app) return;
    int  h   = (dev->resolution >= BSDR_MIN_RESOLUTION &&
                dev->resolution <= BSDR_MAX_RESOLUTION) ? (int)dev->resolution : -1;
    long bps = dev->bitrate > 0 ? dev->bitrate : 0;   /* already bps; 0 = leave unchanged */

    int cur_w, cur_h, cur_br;
    bsdr_app_get_quality(a->app, &cur_w, &cur_h, &cur_br);
    bool changed = (h >= 0 && h != cur_h) || (bps > 0 && (int)bps != cur_br);

    /* width 0 => derive from the desktop aspect ratio (see capture.c) */
    bsdr_app_set_quality(a->app, h >= 0 ? 0 : -1, h, (int)bps);
    BSDR_INFO("bsdr.agent", "headset settings: resolution=%ld bitrate=%ld bps -> %dp%s",
              dev->resolution, dev->bitrate, h, changed ? " (live reconfig)" : "");
    if (changed) a->settings_dirty = 1;  /* main loop applies live via set_quality */

    /* The Quest's PUT /device {isInternetSharing:true|false} is the real enable/disable signal
     * for internet sharing (the agent tick then starts/stops the relay stream to match). */
    if (dev->internet_sharing >= 0) {
        BSDR_INFO("bsdr.agent", "Quest internet sharing -> %s", dev->internet_sharing ? "ON" : "OFF");
        bsdr_app_set_internet_sharing(a->app, dev->internet_sharing == 1);
    }
}

/* multi-Quest registry + selection */
static void on_quest_seen(const char *ip, void *user) {
    agent_t *a = (agent_t *)user;
    if (a->app) bsdr_app_register_quest(a->app, ip);
}
static bool allow_pair_cb(const char *ip, void *user) {
    agent_t *a = (agent_t *)user;
    return a->app ? bsdr_app_quest_allowed(a->app, ip) : true;
}

void bsdr_agent_options_default(bsdr_agent_options *o) {
    memset(o, 0, sizeof(*o));
    o->webui_port = 8088;
    o->open_browser = true;
    o->cloud_latch_burst = 12;   /* comedia keepalive burst on each share start (re-latch) */
    o->cloud_sticky_ports = true; /* reuse ephemeral source ports per relay across toggles (default) */
#ifdef BSDR_HAVE_CAPTURE
    o->video = true;         /* stream the desktop by default on capture builds */
#endif
#ifdef BSDR_HAVE_AUDIO
    o->audio = true;         /* capture system audio + virtual mic by default */
#endif
}

int bsdr_agent_run(const bsdr_agent_options *opt) {
    g_running = 1;
    BSDR_INFO("bsdr.agent", "bsdrX %s starting (Bigscreen Remote Desktop host)", BSDR_VERSION);
    agent_t a;
    memset(&a, 0, sizeof(a));
    a.video           = opt->video;
    a.audio           = opt->audio;
    a.video_file      = opt->video_file;
    a.file_gpu        = opt->file_gpu;
    a.replay_file     = opt->replay_file;
    a.fps             = opt->fps;
    a.bitrate         = opt->bitrate;
    bool control_only = opt->control_only;
    int  webui_port   = opt->webui_port;
    bool open_browser = opt->open_browser;
    const char *quest_ip = opt->quest_ip;

    if (!bsdr_platform_init()) { fprintf(stderr, "platform init failed\n"); return 1; }
    a.lock = bsdr_mutex_new();

    bsdr_app app;
    bsdr_app_init(&app);
    app.audio = a.audio;
    app.max_bitrate = opt->max_bitrate;   /* cap the Quest's bitrate (e.g. hold 1 Mbps when sharing) */
    if (opt->cloud_data)
        snprintf(app.cloud_data_mode, sizeof(app.cloud_data_mode), "%s", opt->cloud_data);
    if (opt->cloud_dtls_role)
        snprintf(app.cloud_dtls_role, sizeof(app.cloud_dtls_role), "%s", opt->cloud_dtls_role);
    app.cloud_latch_burst = opt->cloud_latch_burst;
    app.cloud_src_port = opt->cloud_src_port;
    app.cloud_sticky_ports = opt->cloud_sticky_ports;
    if (opt->no_cloud_video) app.cloud_no_video = true;  /* default: video ON (trailer frags) */
    if (opt->video_decoupled) app.video_decoupled = true; /* default: coupled to the LAN encode */
    if (opt->cpu_only) app.cpu_only = true;               /* default: try CUDA GPU pipeline */
    if (opt->threed_mode) bsdr_app_set_threed(&app, opt->threed_mode,
            opt->threed_deepness > 0 ? opt->threed_deepness : app.threed_deepness,
            opt->threed_convergence, opt->threed_swap,
            opt->threed_full, opt->threed_tier, opt->threed_ai_cmd);  /* --threed (half-SBS unless --threed-full) */
    if (opt->use_vaapi) app.use_vaapi = true;             /* --vaapi: encode on the iGPU */
    if (opt->use_kmsgrab) app.use_kmsgrab = true;         /* --kmsgrab: DRM/KMS capture */
#ifdef BSDR_HAVE_CAPTURE
    g_lan_video_reps = opt->lan_1x ? 1 : 2;               /* --lan-1x: halve the LAN uplink */
    g_lan_sendmmsg = opt->use_sendmmsg ? 1 : 0;           /* --sendmmsg: batch LAN fragment sends */
#endif
    if (opt->no_cloud_audio) app.cloud_no_audio = true;  /* default: audio ON (Opus + 8B trailer) */
    a.app = &app;
    /* Shared overlay handed to every session's capture + input thread: the voice-command balloon
     * (compctl) and the media control bar. It starts hidden; the bar is enabled only while a video
     * file is streaming (lan_live_main), the balloon only while compctl is armed. */
    a.overlay = bsdr_overlay_new();
    if (a.overlay) bsdr_overlay_set_visible(a.overlay, false);
    if (quest_ip) {              /* lock onto one headset; ignore all other IPs */
        bsdr_app_select_quest(&app, quest_ip);
        BSDR_INFO("bsdr.agent", "restricting to Quest %s (ignoring others)", quest_ip);
    }
    /* If a Bigscreen session was saved last run, restore it (validate/renew the token) so we
     * come up already logged in + online; internet sharing then auto-follows the Quest's screen. */
    if (bsdr_app_restore_session(&app))
        BSDR_INFO("bsdr.agent", "Bigscreen: already logged in (auto internet-share armed)");

    /* identity (advertised via discovery) */
    bsdr_discovery_info info;
    memset(&info, 0, sizeof(info));
    bsdr_gen_hex(info.session_id, 48);
    memcpy(info.device_id, info.session_id, 32);
    info.device_id[32] = '\0';
    snprintf(info.version, sizeof(info.version), "0.950.2");
    char host[128] = "bsdrX-host";
#if !defined(_WIN32)
    gethostname(host, sizeof(host));
#endif
    snprintf(info.device_name, sizeof(info.device_name), "%s", host);
    char code[8];
    bsdr_gen_pairing_code(code);
    snprintf(info.pairing_request_code, sizeof(info.pairing_request_code), "%s", code);
    if (opt->on_pairing_code) opt->on_pairing_code(code, opt->user);

    bsdr_discovery *disc = bsdr_discovery_start(&info);
    if (disc) bsdr_discovery_set_on_seen(disc, on_quest_seen, &a);
    bsdr_control_cbs cbs = { 0 };
    cbs.allow_pair = allow_pair_cb;
    cbs.user = &a;
    if (!control_only) {
        cbs.on_start = cb_start;
        cbs.on_stop = cb_stop;
        cbs.on_unpair = cb_unpair;
        cbs.on_settings = cb_settings;
    }
    bsdr_control *ctl = bsdr_control_start(code, &cbs);
    if (!disc || !ctl) { fprintf(stderr, "startup failed (ports in use?)\n"); return 1; }

    /* local control web UI + open the browser */
    bsdr_webui *ui = NULL;
    if (webui_port > 0) {
        ui = bsdr_webui_start(&app, (uint16_t)webui_port);
        if (ui && open_browser) {
            char cmd[128];
#if defined(__APPLE__)
            snprintf(cmd, sizeof(cmd), "open http://127.0.0.1:%d >/dev/null 2>&1 &", webui_port);
#elif defined(_WIN32)
            snprintf(cmd, sizeof(cmd), "start http://127.0.0.1:%d", webui_port);
#else
            snprintf(cmd, sizeof(cmd), "xdg-open http://127.0.0.1:%d >/dev/null 2>&1 &", webui_port);
#endif
            if (system(cmd) != 0) { /* user can open it manually */ }
        }
    }

    BSDR_INFO("bsdr.agent", "========================================================");
    BSDR_INFO("bsdr.agent", " Bigscreen Remote Desktop - %s agent (input channel)",
#if defined(_WIN32)
              "Windows"
#elif defined(__APPLE__)
              "macOS"
#else
              "Linux"
#endif
    );
    /* The code is advertised in the discovery reply and the headset reads it
     * from there automatically — you never type it. Shown only for diagnostics. */
    BSDR_INFO("bsdr.agent", " pairing code (auto-sent in discovery, no need to enter): %s", code);
    BSDR_INFO("bsdr.agent", " discovery udp %d/%d | control http %d | media udp %d",
              BSDR_DISCOVERY_REQUEST_PORT, BSDR_REMOTE_CLIENT_INFO_PORT,
              BSDR_HTTP_SERVER_PORT, BSDR_REMOTE_DESKTOP_PORT);
    BSDR_INFO("bsdr.agent", "========================================================");

    if (ui) BSDR_INFO("bsdr.agent", " CONTROL UI: http://127.0.0.1:%d", webui_port);

    /* Owner-mic sniffer: the Quest never sends its mic to us over the remote-desktop
     * protocol (proven from real captures) — the owner's voice only goes to the room's
     * mediasoup cloud, as plain Opus RTP. Intercept it off the LAN and feed a dedicated
     * BSDR-Quest-OwnerMic. Needs the Quest IP: use --quest_ip, else the paired headset. */
    bsdr_micsniff *sniffer = NULL;
    bool  want_sniff = false, want_mitm = false, have_pw = false;
    char  sniff_pw[128] = {0};
#if !defined(BSDR_PLATFORM_ANDROID)
    bsdr_thread *mic_thr = NULL;               /* local-mic owner source */
    struct local_mic_ctx mic_ctx = { NULL, 0 };
    bsdr_micsub *micsub = NULL;                /* owner-mic cloud substitution (MITM/NFQUEUE) */
#endif
    /* CLI flags seed the desired state; the web UI can flip it live (with a sudo password). */
    if (opt->sniff_mic) { want_sniff = true; want_mitm = opt->sniff_mitm; }

    /* Computer control: the voice pipeline is created lazily on first arm and kept for
     * the process lifetime (only balloon + PCM tap toggle), so session threads never see
     * a freed voice pointer. Its injector is owned here (bsdr_voice_free doesn't own it). */
    bool cc_want = opt->compctl;
    bool cc_vision = opt->compctl_vision;
    bsdr_injector *voice_inj = NULL;
#if defined(BSDR_PLATFORM_ANDROID)
    int cc_active_prev = -1;                /* edge-detect the compctl-active emit to Kotlin */
#endif

    int tick = 0;
    int screen_blanked = 0;
    unsigned my_source_gen = app.source_gen;
    unsigned my_select_gen = bsdr_app_select_gen(&app);
#if defined(BSDR_PLATFORM_ANDROID)
    unsigned my_threed_gen_main = app.threed_gen - 1;   /* force an initial publish */
    unsigned my_fs_gen_main = app.faceswap_gen - 1;
#endif
    while (g_running) {
        bsdr_sleep_ms(200);

        /* Headset picked/changed in the web UI ("Use"): switch the stream to it. If a session is
         * running to a different headset, tear it down; then start streaming to the selected one
         * (its receiver is up once it has been discovered/paired). Empty selection = accept any,
         * leave the current session alone. */
        if (bsdr_app_select_gen(&app) != my_select_gen) {
            my_select_gen = bsdr_app_select_gen(&app);
            char sel[64] = "";
            bsdr_mutex_lock(app.lock);
            snprintf(sel, sizeof sel, "%s", app.selected_quest_ip);
            bsdr_mutex_unlock(app.lock);
            bsdr_mutex_lock(a.lock);
            int running = a.worker != NULL;
            int differ = sel[0] && strcmp(a.remote_ip, sel) != 0;
            bsdr_mutex_unlock(a.lock);
            if (sel[0] && (differ || !running)) {
                BSDR_INFO("bsdr.agent", "headset selected -> streaming to %s", sel);
                teardown_session(&a);
                if (a.app) { bsdr_app_set_paired(a.app, true, "selected headset", sel);
                             bsdr_app_set_streaming(a.app, true); }
                bsdr_mutex_lock(a.lock);
                snprintf(a.remote_ip, sizeof a.remote_ip, "%s", sel);
                spawn_worker_locked(&a);
                bsdr_mutex_unlock(a.lock);
            }
        }

#if !defined(BSDR_PLATFORM_ANDROID)
        /* Source switched in the web UI (desktop <-> webcam <-> stereo <-> file): restart the live
         * session so lan_live_main re-derives the source. Only if a session is actually running (a
         * Quest is streaming); otherwise the next /start picks up the new source on its own. */
        if (app.source_gen != my_source_gen) {
            my_source_gen = app.source_gen;
            bsdr_mutex_lock(a.lock);
            int running = a.worker != NULL && a.remote_ip[0];
            bsdr_mutex_unlock(a.lock);
            if (running) {
                BSDR_INFO("bsdr.agent", "source changed -> restarting live session");
                teardown_session(&a);
                bsdr_mutex_lock(a.lock);
                spawn_worker_locked(&a);
                bsdr_mutex_unlock(a.lock);
            }
        }
#endif

#if defined(BSDR_PLATFORM_ANDROID)
        /* Publish 2D->3D config to the Kotlin GL SBS pipeline independent of a LAN streaming
         * session: on Android the encoder runs (and SBS/neural depth apply) even before a Quest
         * pairs, so the config must reach nativePollThreed from this always-on loop — not only via
         * lan_live_main, which needs an active Quest session. */
        if (app.threed_gen != my_threed_gen_main) {
            my_threed_gen_main = app.threed_gen;
            int m = 0, deep = 0, conv = 0, swap = 0, full = 0, tier = 0;
            bsdr_app_get_threed(&app, &m, &deep, &conv, &swap, &full, &tier, NULL, 0);
            bsdr_android_publish_threed(m, deep, conv, swap, full, tier);
            BSDR_INFO("bsdr.agent", "3D published to GL: mode=%d tier=%d deep=%d", m, tier, deep);
        }
        /* Publish the source choice (desktop/webcam/webcam3d + devices) to the Kotlin capture switch. */
        if (app.source_gen != my_source_gen) {
            my_source_gen = app.source_gen;
            char smode[16] = "", sdev[256] = "", sdev2[256] = "";
            bsdr_app_get_source(&app, smode, sizeof smode, sdev, sizeof sdev);
            bsdr_app_get_source_right(&app, sdev2, sizeof sdev2);
            bsdr_android_publish_source(smode, sdev, sdev2);
            BSDR_INFO("bsdr.agent", "source published to capture: %s dev='%s'%s", smode, sdev,
                      sdev2[0] ? " (+right)" : "");
        }
        /* Publish the face-swap config (enable/tier/source image) to the Kotlin GL faceswap stage. */
        if (app.faceswap_gen != my_fs_gen_main) {
            my_fs_gen_main = app.faceswap_gen;
            bool fson = false; int fstier = 0; char fssrc[512] = "";
            bsdr_app_get_faceswap(&app, &fson, &fstier, fssrc, sizeof fssrc);
            bsdr_android_publish_faceswap(fson ? 1 : 0, fstier, fssrc);
            BSDR_INFO("bsdr.agent", "faceswap published: on=%d tier=%d", fson, fstier);
        }
#endif

        /* Privacy screen-blank reconcile: black out the local monitor while the Quest is connected
         * (so bystanders can't see the desktop), and restore it the instant the Quest disconnects
         * or the operator turns the toggle off. */
        {
            int want = app.blank_want && app.quest_paired;
            if (want != screen_blanked) {
                screen_set_blank(want);
                screen_blanked = want;
                g_screen_blanked = want;   /* keep the signal-handler's view in sync */
                BSDR_INFO("bsdr.agent", "local screen %s",
                          want ? "blanked (Quest viewing)" : "restored");
            }
        }

        /* --- owner-mic sniffer reconcile (CLI + web UI drive `want_sniff`/`want_mitm`) --- */
        {
            bool w, m; char pw[128];
            if (bsdr_app_take_sniff(&app, &w, &m, pw, sizeof pw)) {   /* web UI changed it */
                want_sniff = w; want_mitm = m;
                snprintf(sniff_pw, sizeof sniff_pw, "%s", pw);
                have_pw = pw[0] != 0;
            }
            bool mitm_changed = sniffer && (want_mitm != (bsdr_micsniff_is_mitm(sniffer)));
            if (sniffer && (!want_sniff || mitm_changed)) {
                bsdr_micsniff_stop(sniffer); sniffer = NULL;
                bsdr_app_set_sniff_status(&app, false, want_sniff ? "reconfiguring…" : "off");
            }
            if (want_sniff && !sniffer) {
                char sip[64] = {0};
                if (quest_ip) snprintf(sip, sizeof sip, "%s", quest_ip);
                if (!sip[0]) { bsdr_mutex_lock(a.lock);
                               if (a.remote_ip[0]) snprintf(sip, sizeof sip, "%s", a.remote_ip);
                               bsdr_mutex_unlock(a.lock); }
                if (sip[0]) {
                    int relay = bsdr_app_get_relay_port(&app);   /* web-UI relay port (Android's only path) */
                    if (relay <= 0) relay = opt->sniff_remote_port;
                    bsdr_micsniff_cfg sc = { .quest_ip = sip, .iface = opt->sniff_iface,
                                             .gateway_ip = opt->sniff_gw, .mitm = want_mitm,
                                             .password = have_pw ? sniff_pw : NULL,
                                             .remote_port = relay };
                    sniffer = bsdr_micsniff_start(&sc);
                    memset(sniff_pw, 0, sizeof sniff_pw); have_pw = false;   /* one-shot */
                    if (sniffer) bsdr_app_set_sniff_status(&app, true,
                                     want_mitm ? "active (MITM)" : "active (passive)");
                    else { want_sniff = false;   /* don't hammer sudo every 200ms on failure */
                           bsdr_app_set_sniff_status(&app, false, "failed — check password / permissions"); }
                }
            }
            /* realtime voice change on the owner mic (gender + effects from the web UI) */
            {
                int vg=0, vr=0, ve=0, vw=0; bool vsub=false;
                bsdr_app_get_voicefx(&app, &vg, &vr, &ve, &vw, &vsub);
                if (sniffer) bsdr_micsniff_set_voicefx(sniffer, vg, vr, ve, vw);
#if !defined(BSDR_PLATFORM_ANDROID)
                /* Cloud SUBSTITUTION: rewrite the Quest->cloud owner-mic packets in flight so the room
                 * hears the changed voice. Only possible when we're the MITM (the flow transits us). */
                int can_sub = sniffer && vsub && bsdr_micsniff_is_mitm(sniffer);
                if (can_sub && !micsub) {
                    char qip[64] = "";
                    bsdr_mutex_lock(a.lock); snprintf(qip, sizeof qip, "%s", a.remote_ip); bsdr_mutex_unlock(a.lock);
                    if (!qip[0] && quest_ip) snprintf(qip, sizeof qip, "%s", quest_ip);
                    if (qip[0]) {
                        micsub = bsdr_micsub_start(qip, 4787);
                        if (!micsub) bsdr_app_set_sniff_status(&app, true, "active — substitution unavailable (need root/libnetfilter_queue)");
                    }
                } else if (!can_sub && micsub) {
                    bsdr_micsub_stop(micsub); micsub = NULL;
                }
                if (micsub) bsdr_micsub_set_voicefx(micsub, vg, vr, ve, vw);
#endif
            }
        }
        /* --- computer-control reconcile (gated on the owner mic + an LLM endpoint) --- */
        {
            bool cw, cv;
            if (bsdr_app_take_compctl(&app, &cw, &cv)) { cc_want = cw; cc_vision = cv; }  /* web UI */
            char se[256], st[256], sm[256], le[256], lt[256], lm[256];
            bsdr_app_get_voice(&app, se, st, sm, le, lt, lm, 256);
            bool have_llm = le[0] != 0;
#if defined(BSDR_PLATFORM_ANDROID)
            bool armable = cc_want && have_llm;                     /* device mic is the voice source */
#else
            /* Owner-mic source, in priority: the LAN sniffer / router companion; else this computer's
             * own microphone (owner_mic_local); else — as a WiFi fallback — the cloud room audio. */
            bool cloud_fb = (sniffer == NULL) && app.cloud_mic_fallback && app.internet_sharing;
            bool local_mic = (sniffer == NULL) && app.owner_mic_local;
            bool armable = cc_want && have_llm && (sniffer != NULL || local_mic || cloud_fb);
#endif
            if (armable) {
                if (!a.voice) {                                    /* create once, keep for the run */
                    if (!voice_inj) voice_inj = bsdr_injector_create(1920, 1080);
                    if (voice_inj) {
                        bsdr_voice_config vc0; memset(&vc0, 0, sizeof vc0);
                        a.voice = bsdr_voice_new(&vc0, voice_inj);
                        if (a.voice) {
#if defined(BSDR_PLATFORM_ANDROID)
                            bsdr_voice_set_state_cb(a.voice, voice_state_android, NULL);
                            bsdr_voice_set_feedback_cb(a.voice, voice_feedback_android, NULL);
#else
                            bsdr_voice_set_state_cb(a.voice, voice_state_map, a.overlay);
                            bsdr_voice_set_feedback_cb(a.voice, voice_feedback, a.overlay);
#endif
                            bsdr_voice_set_shot_cb(a.voice, voice_screenshot, NULL);
                        }
                    } else {
                        bsdr_app_set_compctl_status(&app, false, "input device unavailable (uinput)");
                    }
                }
                if (a.voice) {
                    bsdr_voice_config vc; memset(&vc, 0, sizeof vc);
                    snprintf(vc.stt.endpoint, sizeof vc.stt.endpoint, "%s", se);
                    snprintf(vc.stt.token,    sizeof vc.stt.token,    "%s", st);
                    snprintf(vc.stt.model,    sizeof vc.stt.model,    "%.63s", sm);
                    snprintf(vc.llm.endpoint, sizeof vc.llm.endpoint, "%s", le);
                    snprintf(vc.llm.token,    sizeof vc.llm.token,    "%s", lt);
                    snprintf(vc.llm.model,    sizeof vc.llm.model,    "%.63s", lm);
                    vc.vision = cc_vision;
                    vc.max_ms     = opt->listen_max_sec > 0 ? opt->listen_max_sec * 1000 : 0;  /* 0 => 5 min */
                    vc.confirm_ms = opt->confirm_sec   > 0 ? opt->confirm_sec   * 1000 : 0;  /* 0 => 1 min */
                    snprintf(vc.system_prompt, sizeof vc.system_prompt, "%s%s",
                             COMPCTL_SYSTEM_PROMPT, cc_vision ? COMPCTL_VISION_NOTE : "");
                    bsdr_voice_update_config(a.voice, &vc);
#if defined(BSDR_PLATFORM_ANDROID)
                    if (sniffer) {   /* owner mic via the router companion relay (+ voice FX) */
                        bsdr_micsniff_set_pcm_sink(sniffer, voice_pcm_sink, a.voice);
                        bsdr_android_set_voice(NULL);              /* relay is the source, not the device mic */
                    } else {
                        bsdr_android_set_voice(a.voice);           /* device-mic PCM + bubble taps route here */
                    }
                    if (cc_active_prev != 1) { bsdr_android_emit_compctl_active(1); cc_active_prev = 1; }
                    bsdr_app_set_compctl_status(&app, true, sniffer ? "armed (relay owner mic) — tap the voice bubble"
                                                                    : "armed — tap the voice bubble to talk");
#else
                    if (sniffer) {                                 /* LAN sniffer / router companion */
                        bsdr_micsniff_set_pcm_sink(sniffer, voice_pcm_sink, a.voice);
                        bsdr_app_set_room_pcm_sink(&app, NULL, NULL);
                        if (mic_thr) { mic_ctx.stop = 1; bsdr_thread_join(mic_thr); mic_thr = NULL; }
                        bsdr_app_set_compctl_status(&app, true, "armed — click the balloon in VR to talk");
                    } else if (local_mic) {                        /* this computer's microphone */
                        bsdr_app_set_room_pcm_sink(&app, NULL, NULL);
                        if (!mic_thr) { mic_ctx.voice = a.voice; mic_ctx.stop = 0;
                                        mic_thr = bsdr_thread_start(local_mic_main, &mic_ctx); }
                        bsdr_app_set_compctl_status(&app, true, "armed (this computer's mic) — click the balloon");
                    } else {                                       /* cloud room fallback (WiFi) */
                        bsdr_app_set_room_pcm_sink(&app, voice_pcm_sink, a.voice);
                        bsdr_app_set_compctl_status(&app, true, "armed (owner mic via cloud room) — click the balloon");
                    }
                    if (a.overlay) bsdr_overlay_set_balloon(a.overlay, true);
#endif
                }
            } else {                                               /* disarm / not ready */
#if defined(BSDR_PLATFORM_ANDROID)
                bsdr_android_set_voice(NULL);
                if (sniffer) bsdr_micsniff_set_pcm_sink(sniffer, NULL, NULL);
                if (cc_active_prev != 0) { bsdr_android_emit_compctl_active(0); cc_active_prev = 0; }
                if (cc_want && !have_llm)
                    bsdr_app_set_compctl_status(&app, false, "set an LLM endpoint first");
                else
                    bsdr_app_set_compctl_status(&app, false, "off");
#else
                if (a.overlay) bsdr_overlay_set_balloon(a.overlay, false);
                if (sniffer) bsdr_micsniff_set_pcm_sink(sniffer, NULL, NULL);
                bsdr_app_set_room_pcm_sink(&app, NULL, NULL);
                if (mic_thr) { mic_ctx.stop = 1; bsdr_thread_join(mic_thr); mic_thr = NULL; }
                if (cc_want && sniffer == NULL && !app.owner_mic_local && !(app.cloud_mic_fallback && app.internet_sharing))
                    bsdr_app_set_compctl_status(&app, false, "waiting for the owner mic — enable owner-mic sniff/MITM, this computer's mic, or the cloud fallback");
                else if (cc_want && !have_llm)
                    bsdr_app_set_compctl_status(&app, false, "set an LLM endpoint first");
                else
                    bsdr_app_set_compctl_status(&app, false, "off");
#endif
            }
        }
        /* Voice-activity duck for the cloud owner-mic fallback: isolate the owner (loudest room
         * stream) only while a voice command is being captured, then restore the full room mix. */
        app.cloud_mic_duck = (a.voice && bsdr_voice_state_get(a.voice) == BSDR_VST_LISTENING) ? 1 : 0;

        /* The live session (lan_live_main) polls the web-UI region/quality on its
         * own thread and reconfigs the encoder in place — no main-loop push. */
        if (bsdr_app_take_disconnect(&app)) {    /* operator dropped the Quest */
            char dropped[64];
            bsdr_mutex_lock(a.lock);
            snprintf(dropped, sizeof(dropped), "%s", a.remote_ip);
            a.remote_ip[0] = '\0';               /* don't auto-respawn to it */
            bsdr_mutex_unlock(a.lock);
            bsdr_control_force_unpair(ctl);
            if (dropped[0]) bsdr_app_block_quest(&app, dropped);  /* ignore until reselected */
            bsdr_app_set_paired(&app, false, NULL, NULL);
            bsdr_app_set_internet_sharing(&app, false);   /* Quest gone → stop sharing */
            teardown_session(&a);
            BSDR_INFO("bsdr.agent", "disconnected from Quest %s (operator request)", dropped);
        }
        if (tick % 5 == 0)                        /* ~1 s: reconcile internet sharing promptly */
            bsdr_app_cloud_tick(&app);            /* start/stop the relay stream to match desired */
        if (++tick >= 25) {                       /* ~5 s heartbeat-expiry check */
            tick = 0;
            if (bsdr_control_expire_stale(ctl)) {
                bsdr_app_set_paired(&app, false, NULL, NULL);
                bsdr_app_set_internet_sharing(&app, false);   /* heartbeat lost → stop sharing */
                teardown_session(&a);
            }
        }
    }

    BSDR_INFO("bsdr.agent", "shutting down");
    if (screen_blanked) screen_set_blank(0);   /* never exit with the local screen left black */
    g_screen_blanked = 0;
    if (sniffer) bsdr_micsniff_stop(sniffer);
#if !defined(BSDR_PLATFORM_ANDROID)
    if (mic_thr) { mic_ctx.stop = 1; bsdr_thread_join(mic_thr); mic_thr = NULL; }
    if (micsub) { bsdr_micsub_stop(micsub); micsub = NULL; }
#endif
    teardown_session(&a);
    if (a.voice) bsdr_voice_free(a.voice);
    if (voice_inj) bsdr_injector_destroy(voice_inj);   /* voice borrows it; we own it */
    if (a.overlay) bsdr_overlay_free(a.overlay);
    if (ui) bsdr_webui_stop(ui);
    bsdr_discovery_stop(disc);
    bsdr_control_stop(ctl);
    bsdr_app_free(&app);
    bsdr_mutex_free(a.lock);
    bsdr_platform_cleanup();
    return 0;
}

/* ---- CLI entry: parse argv -> options, then run the agent -----------------
 * Excluded from the Android build (BSDR_NO_CLI_MAIN), where the JNI bridge
 * calls bsdr_agent_run() directly. */
#ifndef BSDR_NO_CLI_MAIN
int main(int argc, char **argv) {
    /* Privileged owner-mic helper: the agent re-execs itself (via sudo) with this flag to do the
     * root-only capture/ARP setup, then hands the fd back to the unprivileged parent. Must be
     * handled before anything else — it never runs the normal agent. */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--sniff-helper") == 0)
            return bsdr_micsniff_helper_main(argc, argv);

    bsdr_agent_options opt;
    bsdr_agent_options_default(&opt);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("bsdr_agent (bsdrX) %s\n", BSDR_VERSION);
            return 0;
        }
        else if (strcmp(argv[i], "--debug") == 0) bsdr_log_set_level(BSDR_LOG_DEBUG);
        else if (strcmp(argv[i], "--insecure-tls") == 0) bsdr_tls_set_insecure(true);
        else if (strcmp(argv[i], "--control-only") == 0) opt.control_only = true;
        else if (strcmp(argv[i], "--no-video") == 0) opt.video = false;
        else if (strcmp(argv[i], "--no-audio") == 0) opt.audio = false;
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) opt.video_file = argv[++i];
        else if (strcmp(argv[i], "--file-gpu") == 0) opt.file_gpu = true;
        else if (strcmp(argv[i], "--threed") == 0 && i + 1 < argc) opt.threed_mode = bsdr_threed_mode_parse(argv[++i]);
        else if (strcmp(argv[i], "--threed-deepness") == 0 && i + 1 < argc) opt.threed_deepness = atoi(argv[++i]);
        else if (strcmp(argv[i], "--threed-convergence") == 0 && i + 1 < argc) opt.threed_convergence = atoi(argv[++i]);
        else if (strcmp(argv[i], "--threed-swap") == 0) opt.threed_swap = 1;
        else if (strcmp(argv[i], "--threed-full") == 0) opt.threed_full = 1;
        else if (strcmp(argv[i], "--threed-ai") == 0 && i + 1 < argc) { opt.threed_ai_cmd = argv[++i]; if (!opt.threed_mode) opt.threed_mode = BSDR_3D_AI; }
        else if (strcmp(argv[i], "--threed-tier") == 0 && i + 1 < argc) { opt.threed_tier = (int)bsdr_depth_tier_parse(argv[++i]); if (!opt.threed_mode) opt.threed_mode = BSDR_3D_AI; }
        else if (strcmp(argv[i], "--threed-model-dir") == 0 && i + 1 < argc) {
#ifdef _WIN32
            _putenv_s("BSDR_MODEL_DIR", argv[++i]);   /* mingw has no setenv */
#else
            setenv("BSDR_MODEL_DIR", argv[++i], 1);
#endif
        }
        else if (strcmp(argv[i], "--threed-model-import") == 0 && i + 1 < argc) { int nimp = bsdr_model_import_zip(argv[++i]); fprintf(stderr, "imported %d model(s) from %s\n", nimp < 0 ? 0 : nimp, argv[i]); exit(nimp > 0 ? 0 : 1); }
        else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) opt.replay_file = argv[++i];
        else if (strcmp(argv[i], "--quest_ip") == 0 && i + 1 < argc) opt.quest_ip = argv[++i];
        else if (strcmp(argv[i], "--ui-port") == 0 && i + 1 < argc) opt.webui_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-ui") == 0) opt.webui_port = 0;
        else if (strcmp(argv[i], "--no-browser") == 0) opt.open_browser = false;
        else if (strcmp(argv[i], "--no-cloud-video") == 0) opt.no_cloud_video = true;
        else if (strcmp(argv[i], "--video-decoupled") == 0) opt.video_decoupled = true;
        else if (strcmp(argv[i], "--cpu") == 0) opt.cpu_only = true;
        else if (strcmp(argv[i], "--lan-1x") == 0) opt.lan_1x = true;
        else if (strcmp(argv[i], "--vaapi") == 0) opt.use_vaapi = true;
        else if (strcmp(argv[i], "--kmsgrab") == 0) opt.use_kmsgrab = true;
        else if (strcmp(argv[i], "--sendmmsg") == 0) opt.use_sendmmsg = true;
        else if (strcmp(argv[i], "--no-cloud-audio") == 0) opt.no_cloud_audio = true;
        else if (strcmp(argv[i], "--sniff-mic") == 0) opt.sniff_mic = true;
        else if (strcmp(argv[i], "--sniff-mitm") == 0) { opt.sniff_mic = true; opt.sniff_mitm = true; }
        else if (strcmp(argv[i], "--sniff-iface") == 0 && i + 1 < argc) opt.sniff_iface = argv[++i];
        else if (strcmp(argv[i], "--sniff-gw") == 0 && i + 1 < argc) opt.sniff_gw = argv[++i];
        else if (strcmp(argv[i], "--sniff-remote") == 0 && i + 1 < argc) { opt.sniff_remote_port = atoi(argv[++i]); opt.sniff_mic = true; }
        else if (strcmp(argv[i], "--compctl") == 0) opt.compctl = true;
        else if (strcmp(argv[i], "--compctl-vision") == 0) { opt.compctl = true; opt.compctl_vision = true; }
        else if (strcmp(argv[i], "--listen-max") == 0 && i + 1 < argc) opt.listen_max_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--confirm-timeout") == 0 && i + 1 < argc) opt.confirm_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-bitrate") == 0 && i + 1 < argc) opt.max_bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cloud-data") == 0 && i + 1 < argc) opt.cloud_data = argv[++i];
        else if (strcmp(argv[i], "--cloud-dtls-role") == 0 && i + 1 < argc) opt.cloud_dtls_role = argv[++i];
        else if (strcmp(argv[i], "--cloud-latch-burst") == 0 && i + 1 < argc) opt.cloud_latch_burst = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cloud-src-port") == 0 && i + 1 < argc) opt.cloud_src_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cloud-sticky-ports") == 0) opt.cloud_sticky_ports = true;
        else if (strcmp(argv[i], "--cloud-no-sticky-ports") == 0) opt.cloud_sticky_ports = false;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf(
"bsdr_agent (bsdrX) " BSDR_VERSION " — Bigscreen Remote Desktop host for Linux\n"
"\n"
"Streams this PC's desktop to a paired Bigscreen VR headset over the LAN, with\n"
"audio both ways and remote mouse/keyboard. Pair from the headset's Remote\n"
"Desktop app — the pairing code is shown at startup and broadcast automatically.\n"
"\n"
"USAGE\n"
"  bsdr_agent [options]\n"
"\n"
"SOURCE (what to stream)\n"
"  (default)            Capture the whole desktop (live H.264).\n"
"  --file PATH          Stream a video file instead of the desktop, with an in-VR media bar\n"
"                       (play/pause, seek, volume, exit) and the file's own audio. Re-encoded\n"
"                       so the bar composites; loops at end.\n"
"  --file-gpu           Encode the --file stream on the GPU (NVENC) instead of libx264\n"
"                       (default). libx264 usually looks better at the Quest's low bitrates;\n"
"                       use this to offload the CPU.\n"
"                       --file/--file-gpu also accept http/https/rtsp URLs.\n"
"  --threed MODE        Real-time 2D->3D side-by-side. MODE = off|fast|ai. 'fast' is a\n"
"                       light built-in depth heuristic (good on old laptops); 'ai' uses the\n"
"                       external helper from --threed-ai. Forces the CPU-scale path. Set your\n"
"                       Bigscreen screen to SBS 3D to view it.\n"
"  --threed-deepness N  Depth amount 0..100 (default 35).\n"
"  --threed-convergence N   Screen-plane bias -50..50 (comfort/eye-strain tuning).\n"
"  --threed-swap        Swap left/right eyes.\n"
"  --threed-full        Full resolution per eye: render 3D at 2x resolution (same screen shape).\n"
"                       Sharper but ~4x the pixels (high CPU + bandwidth). Default: light half-SBS.\n"
"  --threed-ai CMD      External depth-estimator command for --threed ai (implies ai mode).\n"
"  --threed-tier T      In-process (built-in) AI depth, no external helper. T = cpu|gpu|hi:\n"
"                       cpu = Depth-Anything-V2-Small; gpu/hi = MiDaS (small/large GPU). The\n"
"                       model is fetched on first use (or import it, below). Implies ai mode.\n"
"  --threed-model-dir D Directory to cache/read depth models (default: per-OS cache dir).\n"
"  --threed-model-import ZIP  Import depth model(s) from a distributed zip into the cache, then exit-safe.\n"
"  --no-video           Don't stream video (input/control only).\n"
"  --no-audio           Don't stream desktop audio or expose the headset mic.\n"
"\n"
"HEADSET\n"
"  --quest_ip IP        Only pair with this headset; ignore all others.\n"
"  --control-only       Run discovery + control + web UI, but never stream.\n"
"\n"
"OWNER MIC (intercept the headset owner's voice)\n"
"  --sniff-mic          Sniff the Quest's room mic (plain Opus RTP to the cloud, which\n"
"                       it never sends us) and expose it as the BSDR-Quest-OwnerMic\n"
"                       source — owner-only, distinct from the room-wide BSDR-Quest-Mic.\n"
"                       Uses --quest_ip if set, else the paired headset. The privileged\n"
"                       capture runs in a helper started via sudo (prompts on the terminal);\n"
"                       can also be toggled live from the web panel. Also runnable as root.\n"
"  --sniff-mitm         Also ARP-spoof Quest<->gateway so a switched LAN routes the\n"
"                       mic through us (toggles ip_forward; heals ARP on exit). Implies --sniff-mic.\n"
"  --sniff-iface IF     Capture interface (default: the default-route interface).\n"
"  --sniff-gw IP        Gateway IP for --sniff-mitm (default: the default-route gateway).\n"
"  --sniff-remote PORT  Receive the owner mic from a router companion (bsdr_micrelay) on this\n"
"                       UDP port instead of capturing locally. Works over WiFi (the router sees\n"
"                       the headset's traffic); no root/MITM. Run bsdr_micrelay on the router:\n"
"                       bsdr_micrelay --iface br-lan --quest <headset-ip> --to <this-host>:PORT\n"
"\n"
"VOICE COMPUTER CONTROL\n"
"  --compctl            Arm voice-driven desktop control: a movable balloon is drawn over\n"
"                       the desktop in VR; click it to speak, and the LLM runs the action.\n"
"                       Requires the owner mic on (--sniff-mic/--sniff-mitm) and an LLM\n"
"                       endpoint (set in the web panel). STT falls back to a free keyless\n"
"                       online service if none is configured. Also toggleable in the web panel.\n"
"  --compctl-vision     Also offer the model a take_screenshot tool so a vision-capable\n"
"                       model can look at the desktop when a request needs it. Implies --compctl.\n"
"  --listen-max SEC     Listening ceiling per command (default 300 = 5 min; silence ends sooner).\n"
"  --confirm-timeout SEC  Auto-cancel the Send/Cancel prompt after this long (default 60).\n"
"\n"
"WEB CONTROL PANEL\n"
"  --ui-port N          Port for the local control panel (default 8088).\n"
"  --no-ui              Disable the web control panel.\n"
"  --no-browser         Don't auto-open the panel in a browser at startup.\n"
"\n"
"INTERNET SHARING (cloud relay)\n"
"  --no-cloud-video         Don't relay video to the room (default: on).\n"
"  --no-cloud-audio         Don't relay desktop audio to the room (default: on).\n"
"  --video-decoupled        Relay runs its own capture+encoder instead of reusing\n"
"                           the LAN encode (2x cost; default: coupled).\n"
"  --cloud-data MODE        Data channel transport: 'raw' (SCTP-over-UDP, the\n"
"                           default, as the official client uses), 'dtls', or 'both'.\n"
"  --cloud-dtls-role ROLE   DTLS role for the data channel: 'client', 'server',\n"
"                           or omit for auto (try client then server).\n"
"  --cloud-latch-burst N    Comedia keepalives sent on each share start to (re)latch\n"
"                           the relay onto our source (default 12; 3=old, 0=off).\n"
"  --cloud-src-port N       Hard-pin local UDP source ports for cloud media (video=N,\n"
"                           audio=N+1, data=N+2). Overrides sticky ports (0=off).\n"
"  --cloud-no-sticky-ports  Disable sticky source ports. By default bsdrX keeps its\n"
"                           ephemeral source ports per relay IP across share toggles\n"
"                           (like the official client) so the relay's comedia latch\n"
"                           stays valid; this flag reverts to fresh ephemeral each time.\n"
"\n"
"PERFORMANCE\n"
"  --cpu                    Force CPU scale/convert (default: try the CUDA GPU\n"
"                           pipeline, hwupload+scale_cuda, and fall back to CPU).\n"
"  --lan-1x                 Send LAN video once instead of 2x — halves the uplink\n"
"                           on a weak WiFi link (default: 2x, like the official host).\n"
"  --vaapi                  Encode on the iGPU via VAAPI instead of NVENC (frees the\n"
"                           dGPU; AMD needs mesa radeonsi). Falls back to CPU on failure.\n"
"  --kmsgrab                Capture via DRM/KMS instead of x11grab (zero-copy with\n"
"                           --vaapi). Needs CAP_SYS_ADMIN: setcap cap_sys_admin+ep build/bsdr_agent.\n"
"  --sendmmsg               Batch each NAL's UDP fragments into one syscall (less overhead).\n"
"  --max-bitrate BPS        Hard cap the encode bitrate, overriding the headset.\n"
"\n"
"DIAGNOSTICS\n"
"  --replay FILE        Replay captured 45002 frames verbatim (protocol testing).\n"
"  --insecure-tls       Skip TLS certificate verification on cloud/API connections\n"
"                       (default: verify against the system trust store). Only for\n"
"                       self-signed dev proxies or offline testing — exposes the login\n"
"                       to man-in-the-middle otherwise.\n"
"  -v, --debug          Verbose: trace pairing, control requests, and packets.\n"
"  -h, --help           Show this help and exit.\n"
"\n"
"NOTES\n"
"  Ports: video 45002, audio 45003, input 45004 (DTLS), discovery 45000,\n"
"  control 45678. Resolution, bitrate, and FPS are chosen live by the headset.\n"
"  Open the web panel (http://127.0.0.1:8088) to log in to Bigscreen, pick a\n"
"  single window to share, or enable internet sharing.\n");
            return 0;
        }
    }

    signal(SIGINT, on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, on_sigint);   /* `kill` should also clean up the virtual devices */
#endif
    atexit(bsdr_audio_cleanup_stale_devices);   /* backstop for any normal exit() path */
    int rc = bsdr_agent_run(&opt);
    bsdr_audio_cleanup_stale_devices();         /* belt-and-suspenders on clean shutdown */
    return rc;
}
#endif /* BSDR_NO_CLI_MAIN */
