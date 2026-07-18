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
#include "bsdr/term.h"
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
#include "bsdr/appwindow.h"
#include "bsdr/plugin.h"
#include "bsdr/agentlib.h"
#include "bsdr/micsniff.h"
#include "bsdr/relayproto.h"
#include "bsdr/overlay.h"
#include "bsdr/threed.h"
#include "bsdr/depth.h"
#include "bsdr/model_store.h"
#include "bsdr/micsub.h"
#include "bsdr/voicestore.h"
#include "bsdr/voiceai.h"
#include "bsdr/voice.h"
#include "bsdr/botprompt.h"
#include "bsdr/updatecheck.h"
#include "bsdr/roomcmd.h"
#include "bsdr/httpc.h"
#if defined(BSDR_PLATFORM_ANDROID)
#include "bsdr_android.h"          /* device-mic voice bridge (no sniffer on Android) */
#endif
#include "bsdr/screenshot.h"
#include "bsdr/screenblank.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#  include <sys/uio.h>     /* struct iovec for --sendmmsg batching */
#endif
#if defined(__linux__) && !defined(BSDR_PLATFORM_ANDROID)
#  include <sys/resource.h>   /* setpriority: raise scheduler priority for the encode path */
#endif

struct lan_input_ctx;   /* fwd decl: agent_t holds a pointer to the live input thread's ctx (defined below) */

typedef struct {
    bool video, audio;
    const char *video_file;     /* --file: stream an H.264 file instead of the desktop */
    const char *replay_file;    /* --replay: stream captured 45002 frames verbatim (diagnostic) */
    int fps, bitrate;

    bsdr_mutex *lock;
    bsdr_thread *worker;
    volatile int stop;          /* per-session cancel/stop flag */
    char remote_ip[64];
    /* Warm-resume across a room change: on /unpair we keep the (expensive VAAPI) video worker RUNNING for
     * a short grace instead of tearing it down, so a re-/start from the SAME headset (a room switch, same
     * IP) resumes instantly — we just force a fresh keyframe and the input thread re-accepts DTLS. 0 = not
     * warm; else the monotonic-ms deadline after which the main loop finalises the teardown. */
    volatile uint64_t warm_until;
    struct lan_input_ctx * volatile input_ctx;  /* the running input thread's ctx, so a warm re-pair can
                                                  * tell it to drop its stale DTLS and re-accept (set while
                                                  * a worker is up; NULL otherwise). */
    int warm_resume;            /* keep the video worker warm across a room-change re-pair (--no-warm-resume off) */
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
    struct bsdr_term * volatile term;  /* active terminal source (headless console); NULL otherwise.
                                        * Written by the video worker, read by the input thread. */
    volatile int term_mode;        /* streaming a terminal source (exit bar active, input -> term) */
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
/* Fatal-crash handler (SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL/SIGQUIT/SIGHUP): the screen-blank is an
 * X11/gamma-ramp change that OUTLIVES the process, so a crash while blanked leaves the monitor black.
 * Restore it as the very first thing, then re-raise with the default disposition so we still crash
 * normally (core dump, correct exit status). Best-effort: screen_set_blank isn't async-signal-safe, but
 * this only runs once, on the way to dying, and an already-black screen is the worse outcome. */
static void on_fatal(int sig) {
    if (g_screen_blanked) { screen_set_blank(0); g_screen_blanked = 0; }
    signal(sig, SIG_DFL);
    raise(sig);
}
void bsdr_agent_stop(void) { g_running = 0; }

/* Fired by the native window's tray "Quit", or by the window closing on a
 * platform without a tray: stop the whole agent. */
static void appwin_on_quit(void *user) { (void)user; bsdr_agent_stop(); }

/* System prompt handed to the LLM for spoken desktop commands. The tool schema
 * (type_text/key/click/scroll/open_app) lives in llm.c; this tells the model how
 * to use it. */
/* micsniff PCM tap -> voice capture buffer (runs on the sniffer thread). */
static void voice_pcm_sink(void *user, const int16_t *pcm, int frames, int channels) {
    bsdr_voice_push_pcm((bsdr_voice *)user, pcm, frames, channels);
}

/* computer-control "speak" tool -> TTS (routed to the cloud room mic or desktop audio). */
static void agent_tts_speak(void *user, const char *text) {
    bsdr_app_tts_say((bsdr_app *)user, text);
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

/* EXPERIMENTAL (P4.6, --ort-arena-off): disable ORT's CPU memory arena on the depth + faceswap
 * sessions — lowers steady RSS for those intermittently-used models. Defined in depth_onnx.c (a core
 * lib file, so tests/Android link it); set here from the CLI, read at session-option setup. */
extern int bsdr_ort_arena_off;

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
    /* XOR bytes [1, flen) with 0x14 (skip byte0 = NAL hdr; trailer stays plaintext). Word-at-a-time
     * over the middle (memcpy = no alignment/aliasing UB, folds to one load/xor/store), scalar tail.
     * Byte-exact with the old per-byte loop — the mask is uniform so endianness is irrelevant. */
    size_t i = 1;
    for (; i + 8 <= flen; i += 8) {
        uint64_t v; memcpy(&v, pkt + i, 8); v ^= 0x1414141414141414ULL; memcpy(pkt + i, &v, 8);
    }
    for (; i < flen; i++) pkt[i] ^= 0x14;
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
        int i = 0;                                  /* XOR the whole Opus payload (byte0 included; word-at-a-time) */
        for (; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, pkt + i, 8); v ^= 0x1414141414141414ULL; memcpy(pkt + i, &v, 8);
        }
        for (; i < n; i++) pkt[i] ^= 0x14;
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
        int i = 0;                                  /* XOR the whole Opus payload (byte0 included; word-at-a-time) */
        for (; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, pkt + i, 8); v ^= 0x1414141414141414ULL; memcpy(pkt + i, &v, 8);
        }
        for (; i < n; i++) pkt[i] ^= 0x14;
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
struct lan_input_ctx { agent_t *a; int sw, sh; volatile int stop; volatile int reaccept; };
static void lan_input_main(void *arg) {
    struct lan_input_ctx *ctx = (struct lan_input_ctx *)arg;
    agent_t *a = ctx->a;
    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, BSDR_REMOTE_DATA_PORT, a->remote_ip, BSDR_REMOTE_DATA_PORT)) {
        BSDR_WARN("bsdr.agent", "LAN input: udp 45004 open failed"); return;
    }
    /* uinput is created ONCE and reused across re-accepts, so a room-change re-pair doesn't churn the
     * kernel devices (which can wedge a real touchpad). Only the DTLS session is re-established. */
    bsdr_injector *inj = bsdr_injector_create(ctx->sw, ctx->sh);
    long n_ev = 0;   /* total injected events across all re-accepts (for the shutdown log below) */
    /* Re-accept loop: a room change unpairs then re-/starts from the same headset. Keep the UDP socket
     * bound and (re)accept a DTLS handshake each time the Quest connects, so input survives a re-pair
     * without a full session respawn (the video worker stays warm across it — see cb_unpair/cb_start). */
    while (!ctx->stop && !a->stop) {
    ctx->reaccept = 0;
    bsdr_dtls *dtls = bsdr_dtls_new(&udp, BSDR_DTLS_SERVER);   /* Quest is the client */
    if (!dtls) break;
    BSDR_INFO("bsdr.agent", "LAN input: DTLS server on 45004, awaiting Quest...");
    if (!bsdr_dtls_handshake(dtls, 30000, &a->stop)) {
        BSDR_WARN("bsdr.agent", "LAN input: DTLS handshake timeout (no Quest on 45004)");
        bsdr_dtls_free(dtls); continue;   /* re-await; the while-condition exits on stop / worker teardown */
    }
    static const uint8_t hello[5] = { 0x10, 0, 0, 0, 0 };       /* host data-channel hello */
    bsdr_dtls_send(dtls, hello, sizeof(hello));
    BSDR_INFO("bsdr.agent", "LAN input: DTLS connected; injecting Quest mouse/keyboard");
    uint8_t buf[2048];
    bsdr_input_event evs[32];
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
    while (!ctx->stop && !a->stop && !ctx->reaccept) {   /* reaccept: a warm re-pair asked us to re-DTLS */
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
            } else if ((a->file_mode || a->term_mode) && ovl && bsdr_overlay_visible(ovl) &&
                       e->kind == BSDR_EV_BUTTON && e->u.button.button == BSDR_BTN_LEFT) {
                /* Exit bar: shown for every non-desktop source. A click on the bar is swallowed; a
                 * click elsewhere falls through (injected / forwarded to the terminal). Playback
                 * controls only mean anything for a file source; a terminal only uses EXIT. */
                if (e->u.button.down) {
                    double val = 0;
                    bsdr_overlay_action act = bsdr_overlay_hit(ovl, last_x, last_y, &val);
                    if (act == BSDR_OVL_EXIT) {   /* "exit" = back to the desktop source (make-before-break), not stop */
                        if (a->app) bsdr_app_set_source(a->app, "desktop", NULL);
                    } else if (a->file_mode) switch (act) {
                        case BSDR_OVL_PLAYPAUSE: a->app->file_paused = !a->app->file_paused; break;
                        case BSDR_OVL_SEEK:      a->app->file_seek_frac = val; a->app->file_seek_gen++; break;
                        case BSDR_OVL_LOOP:      if (a->app) bsdr_app_set_file_loop(a->app, !a->app->file_loop); break;
                        case BSDR_OVL_VOL_DOWN:  { int v = a->app->file_volume - 10; a->app->file_volume = v < 0 ? 0 : v; } break;
                        case BSDR_OVL_VOL_UP:    { int v = val > 0 ? (int)(val * 100) : a->app->file_volume + 10;
                                                   a->app->file_volume = v < 0 ? 0 : v > 100 ? 100 : v; } break;
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
            /* Terminal source: forward to the terminal (XVFB -> XTEST, PTY -> pty bytes) instead of
             * the machine's uinput. a->term is set by the video worker for the active session. */
            struct bsdr_term *term = a->term;
            if (term) bsdr_term_input(term, e);
            else bsdr_injector_handle(inj, e);
            n_ev++;
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
    bsdr_dtls_free(dtls);       /* end this DTLS session; the outer loop re-accepts on the next re-pair */
    }
    if (inj) bsdr_injector_destroy(inj);
    bsdr_udp_close(&udp);
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

/* 1 if the face-swap effect is enabled. The engine itself is now the faceswap PLUGIN (it registers a
 * video-fx and processes each NV12 frame); the core keeps only the config + the encode-path policy —
 * because the plugin's per-frame pixel transform runs on the CPU NV12 frame, so an enabled face swap
 * forces the CPU encode path (like 2D->3D). Android applies face swap in its own GL pipeline, not here. */
static int faceswap_on(agent_t *a) {
#if defined(BSDR_PLATFORM_ANDROID)
    (void)a; return 0;
#else
    if (!a->app) return 0;
    bool on = false;
    bsdr_app_get_faceswap(a->app, &on, NULL, NULL, 0);
    return on ? 1 : 0;
#endif
}

static void lan_live_main(agent_t *a) {
    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, BSDR_REMOTE_DESKTOP_PORT, a->remote_ip, BSDR_REMOTE_DESKTOP_PORT)) {
        BSDR_ERROR("bsdr.agent", "LAN: udp 45002 -> %s failed", a->remote_ip); return;
    }
    if (a->app && a->app->wifi_opt) bsdr_udp_set_dscp(&udp, 32);   /* CS4 video -> WMM AC_VI */
    bsdr_capture_config cfg = {0};
    cfg.fps = (a->app && a->app->fps_cap > 0) ? a->app->fps_cap : (a->fps > 0 ? a->fps : 30);
    cfg.bitrate = a->bitrate > 0 ? a->bitrate : 8000000;
    int user_cpu = a->app && a->app->cpu_only;   /* the operator's --cpu (best-quality software x264) */
    cfg.cpu_only = user_cpu;
    cfg.use_vaapi = a->app && a->app->use_vaapi;
    cfg.use_kmsgrab = a->app && a->app->use_kmsgrab;
    cfg.enc_level = a->app ? a->app->enc_level : 0;   /* encoder effort 0/1/2 (web/--encoder-mode) */
    cfg.enc_x264_threads = a->app ? a->app->enc_x264_threads : 0;   /* opt-in x264 frame threads (P6.9) */
    cfg.force_x11 = a->app && a->app->force_x11;              /* Wayland backend autodetect override */
    cfg.force_pipewire = a->app && a->app->force_pipewire;
    cfg.pw_dmabuf = a->app && a->app->pw_dmabuf;             /* --pw-dmabuf: experimental zero-copy dmabuf->VAAPI */
    /* 2D->3D SBS runs on the CPU NV12 frame, so it needs the CPU-scale path (no VAAPI/CUDA scale
     * and no zero-copy kmsgrab). Force CPU *scale* whenever 3D is enabled — but keep NVENC for the
     * ENCODE (it accepts a software NV12 frame and, unlike x264, the Quest decodes its stream without
     * freezing on the SBS content). Only the operator's own --cpu forces the libx264 encoder. */
    if (threed_on(a)) { cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
    /* --cpu means the FULL software path: CPU scale AND libx264 encode. Without --cpu the desktop
     * auto-picks NVENC (2-pass, p7) even when 3D forced the CPU-scale path above. */
    if (user_cpu) cfg.encoder = "libx264";
    threed_cfg(a, &cfg);   /* capture builds the SBS transform from these cfg fields */
    /* face swap runs on the CPU NV12 frame (like 3D) -> force the CPU scale path when it's on. The
     * processing itself is the faceswap plugin (via the video-fx hook); the core only sets the policy. */
    unsigned my_fs_gen = a->app ? a->app->faceswap_gen : 0;
    if (faceswap_on(a)) { cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
    int rx=0,ry=0,rw=0,rh=0;                           /* capture region; 0s = whole desktop */
    int qw=0,qh=0,qbr=0;                                /* live quality (headset PUT /device) */
    int w=0,h=0; const char *enc="h264";
    bsdr_capture *cap = NULL;
    int termmode = 0;              /* 0 none, 1 xvfb (x11grab), 2 pty (in-process render) */
    bsdr_term *term = NULL;        /* active terminal backend for this session */
    int filemode = (a->video_file != NULL);
    int pl_is = filemode && bsdr_path_is_playlist(a->video_file);   /* .txt = playlist */
    int pl_idx = 0;
    char curpath[512] = "";
    if (filemode) {                                    /* --file / web-UI file source (or .txt playlist) */
        /* Decode+re-encode the file through the capture pipeline (so ANY input codec / definition / fps
         * is normalized to the H.264 the Quest needs) with the in-VR media bar composited on. A .txt
         * source is a playlist; a single file plays once and returns to desktop unless loop is on. A file
         * that can't be opened/decoded falls back to the desktop rather than black-screening the headset. */
        int pl_n = bsdr_playlist_entry(a->video_file, pl_idx, curpath, sizeof curpath);
        if (pl_n == 0) {
            BSDR_WARN("bsdr.agent", "LAN: empty/unreadable file %s -> desktop", a->video_file ? a->video_file : "");
            filemode = 0; pl_is = 0; a->video_file = NULL; a->file_mode = 0;
            if (a->app) bsdr_app_set_source(a->app, "desktop", NULL);
        } else {
            if (a->app) bsdr_app_get_quality(a->app, &qw, &qh, &qbr);
            if (qbr > 0) cfg.bitrate = qbr;
            cfg.out_width = qw > 0 ? qw : 0; cfg.out_height = qh > 0 ? qh : 0;
            /* loop = the file_loop toggle (web UI / overlay): on -> continuous loop, off -> play once then
             * return to desktop (single file) / advance then desktop (playlist). */
            cfg.input_file = curpath; cfg.loop = (a->app && a->app->file_loop) ? 1 : 0;
            /* Default to libx264 (better quality than NVENC at the low bitrates the Quest asks for);
             * --file-gpu opts into NVENC. Both composite the bar via the CPU-scale path. */
            cfg.encoder = a->file_gpu ? NULL : "libx264";
            cap = bsdr_capture_open(&cfg);
            if (!cap) {
                BSDR_WARN("bsdr.agent", "LAN: cannot play file %s (bad codec/definition?) -> desktop", curpath);
                filemode = 0; pl_is = 0; a->video_file = NULL; a->file_mode = 0;
                if (a->app) bsdr_app_set_source(a->app, "desktop", NULL);
            } else {
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
            }
        }
    }
    /* ---- Terminal source: stream a shell (great on a headless box). xvfb = private Xvfb + xterm
     * captured via x11grab with XTEST input; pty = in-process libvterm rendered straight to video
     * (no X). A failure to start/capture falls back to the desktop rather than black-screening. ---- */
    if (!filemode && a->app) {
        char smode[16] = ""; bsdr_app_get_source(a->app, smode, sizeof smode, NULL, 0);
        if (strcmp(smode, "terminal") == 0) {
            char tb[8] = ""; int tc = 0, tr = 0; bsdr_app_get_terminal(a->app, tb, sizeof tb, &tc, &tr);
            char tcmd[512] = ""; bsdr_app_get_source(a->app, NULL, 0, tcmd, sizeof tcmd);
            bsdr_app_get_quality(a->app, &qw, &qh, &qbr);
            bsdr_term_config tcfg = { .backend = strcmp(tb, "xvfb") == 0 ? BSDR_TERM_XVFB : BSDR_TERM_PTY,
                                      .cmd = tcmd[0] ? tcmd : NULL, .cols = tc, .rows = tr,
                                      .width = qw > 0 ? qw : 1280, .height = qh > 0 ? qh : 720,
                                      .desktop = a->app->term_desktop };
            term = bsdr_term_start(&tcfg);
            if (!term) {
                BSDR_WARN("bsdr.agent", "terminal source failed to start -> desktop");
                bsdr_app_set_source(a->app, "desktop", NULL);
            } else {
                if (qbr > 0) cfg.bitrate = qbr;
                if (bsdr_term_is_pty(term)) {
                    int tw2 = 0, th2 = 0; bsdr_term_size(term, &tw2, &th2);
                    cfg.cpu_only = 1; cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; cfg.encoder = "libx264";
                    cfg.raw_render = bsdr_term_render; cfg.raw_user = term; cfg.raw_w = tw2; cfg.raw_h = th2;
                } else {
                    cfg.display = bsdr_term_display(term); cfg.force_x11 = 1; cfg.force_pipewire = 0; cfg.use_kmsgrab = 0;
                    cfg.out_width = qw > 0 ? qw : 0; cfg.out_height = qh > 0 ? qh : 0;
                }
                cap = bsdr_capture_open(&cfg);
                if (!cap) {
                    BSDR_WARN("bsdr.agent", "terminal capture open failed -> desktop");
                    bsdr_term_stop(term); term = NULL;
                    cfg.raw_render = NULL; cfg.raw_user = NULL; cfg.display = NULL;
                    cfg.force_x11 = a->app->force_x11;   /* restore the operator's backend choice */
                    bsdr_app_set_source(a->app, "desktop", NULL);
                } else {
                    termmode = bsdr_term_is_pty(term) ? 2 : 1;
                    a->term = term; a->term_mode = 1;
                    bsdr_capture_info(cap, &w, &h, &enc);
                    if (a->overlay) {
                        bsdr_capture_set_overlay(cap, a->overlay);
                        bsdr_overlay_set_visible(a->overlay, true);
                        bsdr_overlay_set_playing(a->overlay, true);
                        bsdr_overlay_set_position(a->overlay, 0.0, false);   /* exit bar, no seek */
                    }
                    BSDR_INFO("bsdr.agent", "LAN LIVE: terminal(%s) %dx%d via %s -> %s:%d",
                              termmode == 2 ? "pty" : "xvfb", w, h, enc ? enc : "?", a->remote_ip, BSDR_REMOTE_DESKTOP_PORT);
                }
            }
        }
    }
    if (!filemode && !termmode) {
        int is_cam = webcam_cfg(a, &cfg);   /* webcam source -> sets cfg.webcam[_right]; else screen grab */
        if (a->app) bsdr_app_get_region(a->app, &rx, &ry, &rw, &rh);
        cfg.x = rx; cfg.y = ry; cfg.width = rw; cfg.height = rh;
        /* Desktop only: wait briefly for the headset's first PUT /device (its target resolution) before
         * the first frame, so we encode straight at e.g. 1080 instead of streaming our 720p default and
         * then rebuilding the encoder when /device arrives. That mid-stream resolution change forces the
         * Quest to tear down + recreate its decode swapchain, which can crash its compositor (ASW re-init).
         * Bounded so a headset that never sends /device still starts promptly at the default. */
        if (!is_cam && a->app) {
            int waited = 0;
            while (waited < 2500 && g_running && bsdr_app_await_device_pending(a->app)) {
                bsdr_sleep_ms(50); waited += 50;
            }
            if (bsdr_app_await_device_pending(a->app))
                BSDR_INFO("bsdr.agent", "no headset /device in %dms — streaming at the default resolution", waited);
        }
        if (a->app) bsdr_app_get_quality(a->app, &qw, &qh, &qbr);
        if (qbr > 0) cfg.bitrate = qbr;
        cfg.out_width = qw > 0 ? qw : 0; cfg.out_height = qh > 0 ? qh : 0;
        cap = bsdr_capture_open(&cfg);
        if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: %s capture/encode open failed",
                    is_cam ? "webcam" : "desktop");
                    bsdr_udp_close(&udp); return; }
        if (a->overlay) {
            bsdr_capture_set_overlay(cap, a->overlay);   /* voice-command balloon; media bar if non-desktop */
            /* a webcam/stereo source shows the bar so there's always an EXIT-to-desktop control */
            bsdr_overlay_set_visible(a->overlay, is_cam ? true : false);
            if (is_cam) { bsdr_overlay_set_playing(a->overlay, true); bsdr_overlay_set_position(a->overlay, 0.0, false); }
        }
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
    unsigned lan_kf_gen = a->app ? a->app->keyframe_gen : 0;   /* on-demand keyframe generation served */
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
    if (audio_ok && a->app && a->app->wifi_opt) bsdr_udp_set_dscp(&audio_udp, 46);   /* EF audio -> WMM AC_VI/VO */
    struct lan_audio_ctx actx = { a, &audio_udp, adev_ok ? &adev : NULL, 0 };
    bsdr_thread *athr = audio_ok ? bsdr_thread_start(filemode ? lan_file_audio_main : lan_audio_main, &actx)
                                 : NULL;   /* -> Quest:45003 */
#endif
    struct lan_input_ctx ictx = { a, w, h, 0, 0 };    /* Quest mouse/keyboard -> uinput */
    bsdr_thread *ithr = bsdr_thread_start(lan_input_main, &ictx);
    bsdr_mutex_lock(a->lock); a->input_ctx = &ictx; bsdr_mutex_unlock(a->lock);   /* let a warm re-pair signal it */
    unsigned my_seek_gen = a->app->file_seek_gen;
    unsigned my_threed_gen = a->app->threed_gen;
    unsigned my_enc_gen = a->app ? a->app->encoder_gen : 0;
    unsigned my_source_gen = a->app ? a->app->source_gen : 0;
    int cur_loop = (a->app && a->app->file_loop) ? 1 : 0;   /* live "loop file" toggle (web UI / overlay) */
    int cur_cpu = user_cpu;   /* live encoder choice (web-UI CPU<->GPU toggle); user_cpu = the initial */
    while (!a->stop) {
        /* SOURCE switch (web UI desktop<->file<->webcam), MAKE-BEFORE-BREAK: open the new source FIRST
         * and only drop the current capture once the new one is ready, so the desktop keeps streaming
         * until the file's first frame is decoded — no black gap while you pick a file. Same session id
         * / input channel; a fresh keyframe is emitted. A single file's EOF sets the source back to
         * "desktop" (below), which also lands here. */
        if (a->app && a->app->source_gen != my_source_gen) {
            my_source_gen = a->app->source_gen;
            char nmode[16] = "", npath[512] = "", ncurpath[512] = "";
            bsdr_app_get_source(a->app, nmode, sizeof nmode, npath, sizeof npath);
            int nfile = (strcmp(nmode, "file") == 0 && npath[0] != '\0');
            int npl = 0, ready = 1, td = threed_on(a), fson = faceswap_on(a);
            bsdr_capture_config ncfg = cfg;
            ncfg.input_file = NULL; ncfg.webcam = NULL; ncfg.webcam_right = NULL;
            ncfg.x = ncfg.y = ncfg.width = ncfg.height = 0;
            ncfg.raw_render = NULL; ncfg.raw_user = NULL; ncfg.raw_w = ncfg.raw_h = 0;  /* clear a prior pty source */
            ncfg.display = NULL; ncfg.force_x11 = a->app->force_x11;                    /* clear a prior xvfb display */
            int nterm_kind = (strcmp(nmode, "terminal") == 0);
            bsdr_term *nterm = NULL;
            if (nterm_kind) {
                /* Switch TO a terminal source (make-before-break): start the backend, point ncfg at it. */
                char tb[8] = ""; int tc = 0, tr = 0; bsdr_app_get_terminal(a->app, tb, sizeof tb, &tc, &tr);
                char tcmd[512] = ""; bsdr_app_get_source(a->app, NULL, 0, tcmd, sizeof tcmd);
                int nqw=0,nqh=0,nqbr=0; bsdr_app_get_quality(a->app, &nqw, &nqh, &nqbr);
                bsdr_term_config tcfg = { .backend = strcmp(tb,"xvfb")==0?BSDR_TERM_XVFB:BSDR_TERM_PTY,
                                          .cmd = tcmd[0]?tcmd:NULL, .cols = tc, .rows = tr,
                                          .width = nqw>0?nqw:1280, .height = nqh>0?nqh:720,
                                          .desktop = a->app->term_desktop };
                nterm = bsdr_term_start(&tcfg);
                if (!nterm) { BSDR_WARN("bsdr.agent", "source switch: terminal failed to start -> desktop"); ready = 0; }
                else if (bsdr_term_is_pty(nterm)) {
                    int tw2=0,th2=0; bsdr_term_size(nterm, &tw2, &th2);
                    ncfg.cpu_only = 1; ncfg.use_vaapi = 0; ncfg.use_kmsgrab = 0; ncfg.encoder = "libx264";
                    ncfg.raw_render = bsdr_term_render; ncfg.raw_user = nterm; ncfg.raw_w = tw2; ncfg.raw_h = th2;
                    if (nqbr>0) ncfg.bitrate = nqbr;
                } else {
                    ncfg.display = bsdr_term_display(nterm); ncfg.force_x11 = 1; ncfg.force_pipewire = 0; ncfg.use_kmsgrab = 0;
                    ncfg.out_width = nqw>0?nqw:0; ncfg.out_height = nqh>0?nqh:0; if (nqbr>0) ncfg.bitrate = nqbr;
                }
            } else if (nfile) {
                npl = bsdr_path_is_playlist(npath);
                if (bsdr_playlist_entry(npath, 0, ncurpath, sizeof ncurpath) == 0) {
                    BSDR_WARN("bsdr.agent", "source switch: unreadable file %s (keeping current)", npath); ready = 0;
                } else {
                    int nqw=0,nqh=0,nqbr=0; bsdr_app_get_quality(a->app, &nqw, &nqh, &nqbr);
                    ncfg.out_width = nqw>0?nqw:0; ncfg.out_height = nqh>0?nqh:0; if (nqbr>0) ncfg.bitrate = nqbr;
                    ncfg.input_file = ncurpath;
                    ncfg.loop = (a->app->file_loop) ? 1 : 0;                    /* loop toggle; else play once -> desktop */
                    ncfg.cpu_only = 1; ncfg.use_vaapi = 0; ncfg.use_kmsgrab = 0;/* file composites the bar on CPU */
                    ncfg.encoder = a->file_gpu ? NULL : "libx264";
                }
            } else {
                a->video_file = NULL;                                          /* treat as a live source */
                webcam_cfg(a, &ncfg);                                          /* sets ncfg.webcam if webcam mode */
                int nx=0,ny=0,nw=0,nh=0; bsdr_app_get_region(a->app, &nx, &ny, &nw, &nh);
                ncfg.x = nx; ncfg.y = ny; ncfg.width = nw; ncfg.height = nh;
                int nqw=0,nqh=0,nqbr=0; bsdr_app_get_quality(a->app, &nqw, &nqh, &nqbr);
                ncfg.out_width = nqw>0?nqw:0; ncfg.out_height = nqh>0?nqh:0; if (nqbr>0) ncfg.bitrate = nqbr;
                ncfg.cpu_only = cur_cpu || td || fson;
                if (td || fson) { ncfg.use_vaapi = 0; ncfg.use_kmsgrab = 0; }
                else { ncfg.use_vaapi = a->app->use_vaapi; ncfg.use_kmsgrab = a->app->use_kmsgrab; }
                ncfg.encoder = ncfg.cpu_only ? "libx264" : NULL;
            }
            threed_cfg(a, &ncfg);
            bsdr_capture *newcap = ready ? bsdr_capture_open(&ncfg) : NULL;
            if (newcap) {
                bsdr_capture_close(cap); cap = newcap;                          /* make-before-break: drop old now */
                /* Old capture is gone (its render callback / display are no longer used) — now it's safe
                 * to tear down the previous terminal backend and adopt the new one (or none). */
                if (term) { a->term = NULL; a->term_mode = 0; bsdr_term_stop(term); term = NULL; termmode = 0; }
                if (nterm_kind) {
                    term = nterm; a->term = nterm; a->term_mode = 1;
                    termmode = bsdr_term_is_pty(nterm) ? 2 : 1;
                    if (bsdr_term_is_pty(nterm)) { snprintf(a->src_path, sizeof a->src_path, "%s", npath); ncfg.raw_user = nterm; }
                }
                if (nfile) { snprintf(curpath, sizeof curpath, "%s", ncurpath);
                             snprintf(a->src_path, sizeof a->src_path, "%s", npath);
                             ncfg.input_file = curpath; }                       /* point cfg at stable storage */
                cfg = ncfg;
                filemode = nfile; pl_is = nfile && npl; pl_idx = 0;
                a->video_file = nfile ? a->src_path : NULL; a->file_mode = nfile ? 1 : 0;
                if (nfile) { a->app->file_paused = 0; if (a->app->file_volume <= 0) a->app->file_volume = 100; }
                if (a->overlay) {
                    /* Any NON-desktop source (file/webcam/stereo) shows the media bar so there's always
                     * an EXIT-to-desktop control on the LAN side; desktop hides it (voice balloon only). */
                    int nondesktop = (strcmp(nmode, "desktop") != 0);
                    bsdr_capture_set_overlay(cap, a->overlay);
                    bsdr_overlay_set_visible(a->overlay, nondesktop ? true : false);
                    if (nondesktop) {
                        bsdr_overlay_set_playing(a->overlay, true);
                        if (nfile) bsdr_overlay_set_volume(a->overlay, a->app->file_volume);
                        else bsdr_overlay_set_position(a->overlay, 0.0, false);  /* live cam: exit bar, no seek */
                    }
                }
                bsdr_capture_info(cap, &w, &h, &enc);
                tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
#ifdef BSDR_HAVE_AUDIO
                if (athr) { actx.stop = 1; bsdr_thread_join(athr); athr = NULL; actx.stop = 0; }
                if (audio_ok) {                                                /* audio udp is up; swap the source thread */
                    if (!nfile && a->audio && !adev_ok) adev_ok = bsdr_audio_devices_create(&adev);
                    actx.dev = adev_ok ? &adev : NULL;
                    if (nfile || adev_ok) athr = bsdr_thread_start(nfile ? lan_file_audio_main : lan_audio_main, &actx);
                    else BSDR_WARN("bsdr.agent", "source switch: no desktop audio devices");
                }
#endif
                BSDR_INFO("bsdr.agent", "source switch -> %s %dx%d (make-before-break, fresh keyframe)",
                          nfile ? "file" : (ncfg.webcam ? "webcam" : "desktop"), w, h);
            } else {
                /* A FILE that won't open (bad codec/definition/fps, unreadable, decode error) must never
                 * black-screen the headset -> fall back to the desktop source. If desktop itself failed,
                 * keep the current capture. */
                if (nterm) bsdr_term_stop(nterm);   /* backend started but capture failed to open */
                if (nterm_kind) { BSDR_WARN("bsdr.agent", "source switch: terminal failed to open -> desktop");
                             if (a->app) bsdr_app_set_source(a->app, "desktop", NULL); }
                else if (nfile) { BSDR_WARN("bsdr.agent", "source switch: file %s failed to play -> desktop", npath);
                             if (a->app) bsdr_app_set_source(a->app, "desktop", NULL); }
                else BSDR_WARN("bsdr.agent", "source switch: new source failed to open (keeping current)");
            }
        }
        /* Terminal source: the shell exited (user typed `exit`, or the xterm/Xvfb died) -> go back to
         * the desktop rather than freeze on the last frame. The switch above then tears the term down. */
        if (termmode && term && bsdr_term_dead(term)) {
            BSDR_INFO("bsdr.agent", "terminal exited -> desktop");
            if (a->app) bsdr_app_set_source(a->app, "desktop", NULL);
        }
        /* "Loop file" toggled (web UI / overlay) while a single file plays: reopen with the new loop
         * flag so it either self-loops or plays-once->desktop. (A playlist manages its own looping.) */
        if (filemode && !pl_is && a->app && ((a->app->file_loop ? 1 : 0) != cur_loop)) {
            cur_loop = a->app->file_loop ? 1 : 0;
            cfg.loop = cur_loop;
            bsdr_capture_close(cap); cap = bsdr_capture_open(&cfg);
            if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: reopen for loop toggle failed"); break; }
            if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
            bsdr_capture_info(cap, &w, &h, &enc);
            tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
            BSDR_INFO("bsdr.agent", "LAN file loop -> %s (reopen)", cur_loop ? "on" : "off");
        }
        /* Face swap toggled/retuned from the web UI: reconcile the engine (reload model+source) and,
         * because it forces the CPU encode path, reopen the capture. */
        /* Face swap toggled/retuned from the web UI: it forces the CPU encode path (the faceswap plugin's
         * per-frame transform runs on the CPU NV12 frame), so reopen the capture to switch CPU/GPU. The
         * plugin picks up the new enable/source/tier from the core config on its own next refresh. */
        if (a->app && a->app->faceswap_gen != my_fs_gen) {
            my_fs_gen = a->app->faceswap_gen;
            int fs_on = faceswap_on(a), td = threed_on(a);
            cfg.cpu_only = fs_on || td || cur_cpu;
            if (fs_on || td) { cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
            else { cfg.use_vaapi = a->app->use_vaapi; cfg.use_kmsgrab = a->app->use_kmsgrab; }
            if (filemode) cfg.encoder = a->file_gpu ? NULL : "libx264";
            else if (cur_cpu) cfg.encoder = "libx264"; else cfg.encoder = NULL;
            threed_cfg(a, &cfg); if (!filemode) webcam_cfg(a, &cfg);
            bsdr_capture_close(cap); cap = bsdr_capture_open(&cfg);
            if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: reopen for faceswap change failed"); break; }
            if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
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
            cfg.cpu_only = on || cur_cpu;
            if (on) { cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
            else { cfg.use_vaapi = a->app->use_vaapi; cfg.use_kmsgrab = a->app->use_kmsgrab; }
            /* 3D keeps NVENC (Quest decodes it cleanly); only --cpu or file-default forces x264. */
            if (filemode) cfg.encoder = a->file_gpu ? NULL : "libx264";
            else if (cur_cpu) cfg.encoder = "libx264"; else cfg.encoder = NULL;
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
        /* Encoder CPU<->GPU toggled from the web UI: reopen the capture IN PLACE (like the 3D /
         * bitrate reconfig) so the switch is seamless. Doing this here — instead of via a source_gen
         * session restart — keeps the Quest input channel up and emits a fresh keyframe immediately,
         * so the headset resyncs at once instead of freezing for seconds with no input. */
        if (a->app && !filemode && a->app->encoder_gen != my_enc_gen) {
            my_enc_gen = a->app->encoder_gen;
            cur_cpu = a->app->cpu_only;
            int td = threed_on(a), fs_on = faceswap_on(a);
            cfg.cpu_only = cur_cpu || td || fs_on;
            if (td || fs_on) { cfg.use_vaapi = 0; cfg.use_kmsgrab = 0; }
            else { cfg.use_vaapi = a->app->use_vaapi; cfg.use_kmsgrab = a->app->use_kmsgrab; }
            cfg.encoder = cur_cpu ? "libx264" : NULL;   /* NULL -> auto (nvenc, then x264 fallback) */
            threed_cfg(a, &cfg); webcam_cfg(a, &cfg);
            bsdr_capture_close(cap); cap = bsdr_capture_open(&cfg);
            if (!cap) { BSDR_ERROR("bsdr.agent", "LAN: reopen for encoder change failed"); break; }
            if (a->overlay) bsdr_capture_set_overlay(cap, a->overlay);
            bsdr_capture_info(cap, &w, &h, &enc);
            tw = (uint16_t)((w+15)&~15); th = (uint16_t)((h+15)&~15);
            BSDR_INFO("bsdr.agent", "LAN encoder -> %s (in-place reopen, fresh keyframe)",
                      cur_cpu ? "CPU/libx264" : "GPU/NVENC");
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
        /* Serve an on-demand keyframe request (a new cloud joiner / RTCP PLI). In coupled mode this
         * LAN encoder is the cloud's source too, so its IDR reaches the joiner; the Quest just gets a
         * harmless extra keyframe. */
        if (a->app) {
            unsigned g = a->app->keyframe_gen;
            if (g != lan_kf_gen) { bsdr_capture_force_keyframe(cap); lan_kf_gen = g; }
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
        if (r < 0 && filemode && !pl_is) {
            /* single file finished -> return to the desktop source automatically (the source-switch
             * block above reopens make-before-break, so this is seamless). */
            BSDR_INFO("bsdr.agent", "file playback ended -> returning to desktop");
            if (a->app) bsdr_app_set_source(a->app, "desktop", NULL);
            continue;
        }
        if (r <= 0) { if (r < 0) break; bsdr_sleep_ms(2); continue; }
        /* Reflect playback state onto the bar (progress, play/pause icon, volume). */
        if (filemode && a->overlay) {
            int seekable = 0; double pos = bsdr_capture_position(cap, &seekable);
            bsdr_overlay_set_position(a->overlay, pos, seekable);
            bsdr_overlay_set_playing(a->overlay, !a->app->file_paused);
            bsdr_overlay_set_loop(a->overlay, a->app->file_loop != 0);
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
    bsdr_mutex_lock(a->lock); a->input_ctx = NULL; bsdr_mutex_unlock(a->lock);   /* ictx about to go out of scope */
    if (ithr) { ictx.stop = 1; bsdr_thread_join(ithr); }   /* input thread stops reading a->term first */
    if ((filemode || termmode) && a->overlay) bsdr_overlay_set_visible(a->overlay, false);
    a->file_mode = 0; a->term_mode = 0;
    if (cap) bsdr_capture_close(cap);   /* drop the capture before the terminal it renders from */
    if (term) { a->term = NULL; bsdr_term_stop(term); term = NULL; }
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

/* Privacy screen-blank: black out the PHYSICAL monitor at the output gamma LUT (X11 RandR, Windows
 * SetDeviceGammaRamp, macOS CoreGraphics, Wayland wlr-gamma-control) so capture keeps full content for
 * the Quest while the local screen shows black — and, unlike DPMS, it isn't woken by injected input.
 * See src/screenblank.c. */
static void screen_set_blank(int on) { bsdr_screen_blank(on); }

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
    /* WARM RESUME: if the video worker is still up from a very recent unpair (a room change) and the
     * headset IP is unchanged, DON'T teardown+respawn — keep the running (expensive VAAPI) encoder, force
     * a fresh keyframe for the re-joining headset, and tell the input thread to re-accept a DTLS
     * handshake. Cuts the ~3s encoder/DTLS rebuild so the video barely blinks on a room switch. */
    bsdr_mutex_lock(a->lock);
    int warm = a->warm_resume && a->warm_until && a->worker &&
               strcmp(a->remote_ip, dev->remote_ip) == 0;
    a->warm_until = 0;
    struct lan_input_ctx *ic = a->input_ctx;
    bsdr_mutex_unlock(a->lock);
    if (warm) {
        BSDR_INFO("bsdr.agent", "warm resume (same headset %s) — keeping the encoder, forcing a keyframe",
                  dev->remote_ip);
        if (a->app) { bsdr_app_set_paired(a->app, true, dev->device_name, dev->remote_ip);
                      bsdr_app_set_streaming(a->app, true);
                      bsdr_app_request_keyframe(a->app); }
        if (ic) ic->reaccept = 1;   /* input drops its stale DTLS and re-accepts the Quest's new handshake */
        return;
    }
    teardown_session(a);
    if (a->app) {
        bsdr_app_set_paired(a->app, true, dev->device_name, dev->remote_ip);
        bsdr_app_await_device_begin(a->app);   /* wait for the headset's resolution before frame 1 */
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
    /* Tear down the LAN session, but hold the internet-share relay on a grace timer (set_paired(false)
     * arms it) so a quick unpair/re-pair keeps the cloud stream — it's finalized by the main loop's
     * bsdr_app_unpair_grace_expired() if no re-pair arrives. */
    if (a->app) bsdr_app_set_paired(a->app, false, NULL, NULL);
    /* Room change = unpair immediately followed by a re-/start from the same headset. Keep the video
     * worker WARM for a short grace so the re-pair resumes instantly (the cb_start warm path); the main
     * loop finalises the teardown if no re-pair arrives before the deadline. --no-warm-resume tears down
     * now, as before. */
    bsdr_mutex_lock(a->lock);
    int warm_ok = a->warm_resume && a->worker;
    if (warm_ok) a->warm_until = bsdr_now_ms() + 12000;   /* ~12 s grace (re-pair typically ~5 s) */
    bsdr_mutex_unlock(a->lock);
    if (!warm_ok) teardown_session(a);
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

/* Ask the scheduler to favour us: the capture -> H.264 encode -> send loop is latency-sensitive, so a
 * negative nice keeps it responsive under desktop load. Best-effort — a negative nice needs
 * CAP_SYS_NICE (or root); without it the kernel refuses and we just stay at the default, no error. */
static void raise_priority(void) {
#if defined(__linux__) && !defined(BSDR_PLATFORM_ANDROID)
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, -10) == 0)
        BSDR_INFO("bsdr.agent", "process priority raised (nice -10) for the encode/send path");
    else
        BSDR_DEBUG("bsdr.agent", "could not raise priority (%s); grant CAP_SYS_NICE "
                   "(setcap cap_sys_nice+ep build/bsdr_agent) or run niced for smoother encode",
                   strerror(errno));
#endif
}

int bsdr_agent_run(const bsdr_agent_options *opt) {
    g_running = 1;
    BSDR_INFO("bsdr.agent", "bsdrX %s starting (Bigscreen Remote Desktop host)", BSDR_VERSION);
    raise_priority();
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
    a.warm_resume = getenv("BSDR_NO_WARM_RESUME") ? 0 : 1;   /* warm-resume the encoder across a room-change re-pair */

    bsdr_app app;
    bsdr_app_init(&app);
    bsdr_app_load_settings(&app);   /* persisted encoder/bitrate prefs; CLI flags below still override */
    bsdr_plugins_load(&app);        /* loadable plugins (build/plugins or $BSDR_PLUGIN_DIR); optional */
    app.audio = a.audio;
    app.max_bitrate = opt->max_bitrate;   /* cap the Quest's bitrate (e.g. hold 1 Mbps when sharing) */
    if (opt->cloud_data)
        snprintf(app.cloud_data_mode, sizeof(app.cloud_data_mode), "%s", opt->cloud_data);
    if (opt->cloud_dtls_role)
        snprintf(app.cloud_dtls_role, sizeof(app.cloud_dtls_role), "%s", opt->cloud_dtls_role);
    if (opt->bot_mode) {  /* CLI overrides the saved bot presence mode (plugins are already loaded, so
                           * this fires the mode's activation callback). Legacy "full" -> the "fullbot"
                           * plugin mode; anything else is passed through ("audio" or a plugin mode). */
        const char *m = strcmp(opt->bot_mode, "full") == 0 ? "fullbot" : opt->bot_mode;
        bsdr_app_bot_set_mode(&app, m);
    }
    if (opt->encoder_mode)   /* CLI overrides the saved encoder mode: quality|balanced|performance */
        app.enc_level = strcmp(opt->encoder_mode, "performance") == 0 ? 2 :
                        strcmp(opt->encoder_mode, "balanced")    == 0 ? 1 : 0;
    app.cloud_latch_burst = opt->cloud_latch_burst;
    app.cloud_src_port = opt->cloud_src_port;
    app.cloud_sticky_ports = opt->cloud_sticky_ports;
    if (opt->no_cloud_video) app.cloud_no_video = true;  /* default: video ON (trailer frags) */
    if (opt->video_decoupled) app.video_decoupled = true; /* default: coupled to the LAN encode */
    /* Encoder: saved settings (loaded below) set the baseline; an explicit CLI flag still wins.
     * --cpu forces libx264 (CPU scale), --gpu forces the CUDA/NVENC pipeline. */
    if (opt->cpu_only)   app.cpu_only = true;
    if (opt->gpu_encode) app.cpu_only = false;
    if (opt->threed_mode) bsdr_app_set_threed(&app, opt->threed_mode,
            opt->threed_deepness > 0 ? opt->threed_deepness : app.threed_deepness,
            opt->threed_convergence, opt->threed_swap,
            opt->threed_full, opt->threed_tier, opt->threed_ai_cmd);  /* --threed (half-SBS unless --threed-full) */
    if (opt->use_vaapi) app.use_vaapi = true;             /* --vaapi: encode on the iGPU */
    if (opt->use_kmsgrab) app.use_kmsgrab = true;         /* --kmsgrab: DRM/KMS capture */
    if (opt->force_x11) app.force_x11 = true;             /* --x11: force x11grab */
    if (opt->force_pipewire) app.force_pipewire = true;   /* --wayland/--pipewire: force the portal */
    if (opt->pw_dmabuf) app.pw_dmabuf = true;             /* --pw-dmabuf: experimental zero-copy dmabuf->VAAPI */
    app.cloud_rtcp_pli = opt->cloud_rtcp_pli;             /* --cloud-rtcp-pli: force IDR on an SFU keyframe request */
    if (opt->lan_1x) app.lan_1x = true;                   /* --lan-1x forces on; absence keeps the saved value */
    if (opt->fps > 0) app.fps_cap = opt->fps;             /* --fps caps the capture fps (persisted) */
    if (opt->faceswap_detect_every > 0) app.faceswap_detect_every = opt->faceswap_detect_every;  /* opt-in P4.5 */
    if (opt->x264_threads > 0) app.enc_x264_threads = opt->x264_threads;   /* opt-in P6.9 */
#ifdef BSDR_HAVE_CAPTURE
    g_lan_video_reps = app.lan_1x ? 1 : 2;                /* saved/CLI lan-1x; the main loop keeps it live */
#if defined(__linux__) && !defined(BSDR_PLATFORM_ANDROID)
    g_lan_sendmmsg = opt->no_sendmmsg ? 0 : 1;            /* default ON: sendmmsg batches the burst into one syscall */
#else
    g_lan_sendmmsg = opt->use_sendmmsg ? 1 : 0;           /* elsewhere the batch path is a sendto loop -> opt-in only */
#endif
    bsdr_ort_arena_off = opt->ort_arena_off ? 1 : 0;      /* --ort-arena-off (experimental): lower ORT RSS */
#endif
    if (opt->no_cloud_audio) app.cloud_no_audio = true;  /* default: audio ON (Opus + 8B trailer) */
    a.app = &app;
    /* Seed the initial source from the CLI so a headset that pairs picks it up (the session worker
     * reads app.source): --terminal streams a shell, --file a video file; otherwise the desktop. */
    if (opt->terminal) {
        bsdr_app_set_terminal(&app, opt->terminal, opt->terminal_cols, opt->terminal_rows);
        bsdr_app_set_source(&app, "terminal", opt->terminal_cmd ? opt->terminal_cmd : "");
        BSDR_INFO("bsdr.agent", "source: terminal (%s backend)", app.term_backend);
    } else if (opt->video_file) {
        bsdr_app_set_source(&app, "file", opt->video_file);
    }
    /* Shared overlay handed to every session's capture + input thread: the voice-command balloon
     * (compctl) and the media control bar. It starts hidden; the bar is enabled only while a video
     * file is streaming (lan_live_main), the balloon only while compctl is armed. */
    a.overlay = bsdr_overlay_new();
    if (a.overlay) bsdr_overlay_set_visible(a.overlay, false);
    if (quest_ip) {              /* lock onto one headset; ignore all other IPs */
        bsdr_app_select_quest(&app, quest_ip);
        BSDR_INFO("bsdr.agent", "restricting to Quest %s (ignoring others)", quest_ip);
    }
    /* Bring the control web UI up FIRST — before the (slow, blocking) cloud session restore below — so
     * the panel is reachable within a fraction of a second. It runs in its own thread and reflects
     * login/bot state live via /api/status, so it works fine while the cloud calls proceed. */
    bsdr_updatecheck_start(&app);   /* hourly, background, quiet — flags a newer release for the UI */

    bsdr_webui *ui = NULL;
    bsdr_appwindow *appwin = NULL;
    if (webui_port > 0) {
        ui = bsdr_webui_start(&app, (uint16_t)webui_port, opt->webui_bind, opt->webui_allow);
        if (ui && open_browser) {
            /* Open the browser only once the UI actually answers — poll /api/status (up to ~3 s) so the
             * first page load can't race a not-yet-serving server. */
            char probe_url[64]; snprintf(probe_url, sizeof probe_url, "http://127.0.0.1:%d/api/status", webui_port);
            static char probe[8192]; bool reachable = false;
            for (int i = 0; i < 30 && !reachable; i++) {
                if (bsdr_http_request("GET", probe_url, NULL, 0, NULL, NULL, 0, probe, sizeof probe) >= 0) reachable = true;
                else bsdr_sleep_ms(100);
            }
            if (!reachable) BSDR_WARN("bsdr.agent", "web UI not reachable yet; open http://127.0.0.1:%d manually", webui_port);
            if (reachable) {
                /* Open the control panel as a chromeless native "app window" (default) or the plain
                 * default browser (--browser). In app-window mode the window is a tracked child and a
                 * system-tray icon is installed where available (Windows always; Linux when a
                 * StatusNotifier host exists): closing it minimizes to the tray, and the tray menu
                 * offers Open / Quit. Without a tray, closing the window quits the agent. */
                appwin = bsdr_appwindow_start((uint16_t)webui_port, !opt->plain_browser,
                                              appwin_on_quit, NULL);
            }
        }
    }

    /* Bring the LAN remote-desktop path (discovery + control server) up BEFORE the cloud/bot logins
     * below: a Quest that's already running discovers us and starts streaming the moment these are up,
     * and none of the LAN video path needs a Bigscreen login. The (slow, blocking) session restores
     * happen right after, so cloud internet-share / the bot arm a beat later without delaying LAN video. */

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

    /* Now (after the LAN path is live) restore the saved Bigscreen sessions — these do slow, blocking
     * network round-trips (token validate/renew + the bot's WS), and are only needed for cloud
     * internet-share and the second "bot" account, NOT for LAN video. Doing them here keeps a
     * to-a-running-Quest LAN stream from waiting on logins it doesn't need. */
    if (bsdr_app_restore_session(&app))
        BSDR_INFO("bsdr.agent", "Bigscreen: already logged in (auto internet-share armed)");
    bsdr_app_bot_restore(&app);   /* second "bot" account, if one was saved (its own session/WS) */

    /* Owner-mic sniffer: the Quest never sends its mic to us over the remote-desktop
     * protocol (proven from real captures) — the owner's voice only goes to the room's
     * mediasoup cloud, as plain Opus RTP. Intercept it off the LAN and feed a dedicated
     * BSDR_QuestMic. Needs the Quest IP: use --quest_ip, else the paired headset. */
    bsdr_micsniff *sniffer = NULL;
    bool  want_sniff = false, want_mitm = false, have_pw = false;
    int   cur_method = -1;                     /* method the running sniffer was started with */
    char  sniff_pw[128] = {0};
#if !defined(BSDR_PLATFORM_ANDROID)
    bsdr_thread *mic_thr = NULL;               /* local-mic owner source */
    struct local_mic_ctx mic_ctx = { NULL, 0 };
    bsdr_micsub *micsub = NULL;                /* owner-mic cloud substitution (MITM/NFQUEUE) */
#endif
    /* CLI flags seed the desired state; the web UI can flip it live (with a sudo password). The
     * capture method (passive/MITM/relay) lives in the app so the web UI select drives it too. */
    if (opt->sniff_mic) {
        want_sniff = true;
        int m = opt->sniff_remote_port > 0 ? 2 : (opt->sniff_mitm ? 1 : 0);
        bsdr_app_set_sniff_method(&app, m);
        want_mitm = (m == 1);
    }

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
    int follow_ctr = 0;               /* follow-me poll cadence (200ms/iter -> ~15 s) */
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
        /* Source switches (desktop <-> webcam <-> stereo <-> file) are now handled IN-PLACE by
         * lan_live_main (make-before-break, keeps the desktop streaming until the new source is ready)
         * — NOT by a teardown/respawn here, which caused a black gap. When no session is running the
         * next /start derives the source on its own, so there's nothing to do. Just consume the gen. */
        if (app.source_gen != my_source_gen) my_source_gen = app.source_gen;
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

        /* --- Quest-mic sniffer reconcile (CLI + web UI drive want_sniff + the capture method) --- */
        {
            bool w; char pw[128];
            if (bsdr_app_take_sniff(&app, &w, NULL, pw, sizeof pw)) {   /* web UI changed it */
                want_sniff = w;
                snprintf(sniff_pw, sizeof sniff_pw, "%s", pw);
                have_pw = pw[0] != 0;
            }
            int method = bsdr_app_get_sniff_method(&app);   /* 0 passive / 1 MITM / 2 relay */
            want_mitm = (method == 1);
            bool method_changed = sniffer && (method != cur_method);
            if (sniffer && (!want_sniff || method_changed)) {
                bsdr_micsniff_stop(sniffer); sniffer = NULL; cur_method = -1;
                bsdr_app_set_sniff_status(&app, false, want_sniff ? "reconfiguring…" : "off");
            }
            if (want_sniff && !sniffer) {
                char sip[64] = {0};
                if (quest_ip) snprintf(sip, sizeof sip, "%s", quest_ip);
                if (!sip[0]) { bsdr_mutex_lock(a.lock);
                               if (a.remote_ip[0]) snprintf(sip, sizeof sip, "%s", a.remote_ip);
                               bsdr_mutex_unlock(a.lock); }
                if (sip[0]) {
                    /* relay port only applies in relay mode; fall back to the CLI --sniff-remote port,
                     * then to the well-known relay port so auto-discovery (the relay's broadcast beacon
                     * + mic forward both use BSDR_RELAY_PORT) lines up with zero configuration. */
                    int relay = 0;
                    if (method == 2) { relay = bsdr_app_get_relay_port(&app);
                                       if (relay <= 0) relay = opt->sniff_remote_port;
                                       if (relay <= 0) relay = BSDR_RELAY_PORT; }
                    bsdr_micsniff_cfg sc = { .quest_ip = sip, .iface = opt->sniff_iface,
                                             .gateway_ip = opt->sniff_gw, .mitm = want_mitm,
                                             .password = have_pw ? sniff_pw : NULL,
                                             .remote_port = relay };
                    sniffer = bsdr_micsniff_start(&sc);
                    memset(sniff_pw, 0, sizeof sniff_pw); have_pw = false;   /* one-shot */
                    if (sniffer) { cur_method = method;
                                   bsdr_app_set_sniff_status(&app, true,
                                       method == 2 ? "active (relay)" :
                                       method == 1 ? "active (MITM)" : "active (passive)"); }
                    else { want_sniff = false;   /* don't hammer sudo every 200ms on failure */
                           bsdr_app_set_sniff_status(&app, false, "failed — check password / permissions"); }
                }
            }
            /* realtime voice change on the Quest mic (gender + effects from the web UI) */
            {
#ifdef BSDR_HAVE_CAPTURE
                g_lan_video_reps = app.lan_1x ? 1 : 2;   /* live: web toggle halves the LAN uplink */
#endif
                int vg=0, vfm=0, vvol=0, vr=0, ve=0, vw=0; bool vsub=false;
                bsdr_app_get_voicefx(&app, &vg, &vfm, &vvol, &vr, &ve, &vw, &vsub);
                /* GUARD (owner directive): voice-changer / AI-voice SUBSTITUTION — replacing the owner's
                 * voice in the room — may ONLY act on the INTERCEPTED ORIGINAL owner mic (the LAN
                 * sniffer / MITM path). It must NEVER be applied to a RE-CONSUMED cloud stream (the
                 * cloud-mic fallback or room loopback), because that stream is the owner's voice ALREADY
                 * IN THE ROOM — "substituting" it would echo/double it, not replace it. That stream may
                 * feed STT / computer-control / bot functions only. All substitution paths below require
                 * `sniffer`, so this holds structurally; we ALSO force it off + surface a status when the
                 * operator has substitution on but the owner-mic source is not an interceptable original
                 * (cloud fallback or this-PC mic), so the invariant can't be broken by a future change. */
                if (vsub && sniffer == NULL) {
                    vsub = false;
                    bsdr_app_set_sniff_status(&app, false,
                        "voice change can't substitute the cloud/PC-mic source (needs the LAN owner-mic); it still feeds STT");
                }
                (void)vg; (void)vfm; (void)vvol; (void)vr; (void)ve; (void)vw;  /* effects are the voice-changer plugin's now */
                /* Cloud SUBSTITUTION via the RELAY (all platforms incl. Android): bsdrX re-encodes the
                 * changed voice and the router companion forwards it to the cloud in place of the
                 * original. No-op unless the sniffer runs in router-companion mode. */
                if (sniffer) bsdr_micsniff_set_substitute(sniffer, vsub);
                /* Render the owner voice into the BSDR_QuestMic device only when the operator asked for
                 * it (default off); the STT/computer-control tap runs regardless. */
                if (sniffer) bsdr_micsniff_set_questmic_feedback(sniffer, app.owner_mic_to_questmic ? 1 : 0);
#if !defined(BSDR_PLATFORM_ANDROID)
                /* Cloud SUBSTITUTION in flight (LOCAL, no relay): rewrite the Quest->cloud owner-mic
                 * packets as they transit us. Only when we're the MITM (NFQUEUE/WinDivert/macOS BPF). */
                int can_sub = sniffer && vsub && bsdr_micsniff_is_mitm(sniffer);
                if (can_sub && !micsub) {
                    char qip[64] = "";
                    bsdr_mutex_lock(a.lock); snprintf(qip, sizeof qip, "%s", a.remote_ip); bsdr_mutex_unlock(a.lock);
                    if (!qip[0] && quest_ip) snprintf(qip, sizeof qip, "%s", quest_ip);
                    if (qip[0]) {
                        micsub = bsdr_micsub_start(qip, 4787);
                        if (!micsub) bsdr_app_set_sniff_status(&app, true, "active — substitution unavailable (need admin + NFQUEUE/WinDivert)");
                    }
                } else if (!can_sub && micsub) {
                    bsdr_micsub_stop(micsub); micsub = NULL;
                }
#endif
                /* Voice EFFECTS (DSP + AI/RVC) are the voice-changer PLUGIN's job now: it transforms the
                 * PCM inside the mic paths via the media-fx hook, and the substitution plumbing above just
                 * forwards the (already-changed) voice. The old in-core voice-fx/voice-ai reconcile — which
                 * resolved RVC model paths and pushed effect params to the mic modules — is gone with the
                 * engine. The RVC model store itself stays in the core (host services). */
            }
        }
        /* --- computer-control reconcile (gated on the owner mic + an LLM endpoint) --- */
        {
            bool cw, cv;
            if (bsdr_app_take_compctl(&app, &cw, &cv)) { cc_want = cw; cc_vision = cv; }  /* web UI */
            char se[256], st[256], sm[256], le[256], lt[256], lm[256];
            bsdr_app_get_voice(&app, se, st, sm, le, lt, lm, 256);
            bool have_llm = le[0] != 0;
            /* Per-speaker room utterance router (mic-check / one-shot translation): created once and
             * kept for the process lifetime (the bot's room-audio tap feeds it), enabled while the LLM
             * assistant is on. Skipped entirely when a PLUGIN owns the bot (it registered a presence
             * mode) — then the core stays bare and the plugin is the sole brain; the tap routes to the
             * plugin, not here. */
            if (have_llm && cc_want && !app.roomcmd && !bsdr_app_has_plugin_bot(&app)) {
                bsdr_roomcmd *rcmd = bsdr_roomcmd_new(&app);
                bsdr_mutex_lock(app.lock); app.roomcmd = rcmd; bsdr_mutex_unlock(app.lock);
            }
            if (app.roomcmd) bsdr_roomcmd_set_enabled((bsdr_roomcmd *)app.roomcmd, have_llm && cc_want);
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
                            bsdr_voice_set_speak(a.voice, agent_tts_speak, &app);   /* "speak" tool -> TTS */
                            bsdr_voice_set_app(a.voice, &app);   /* owner balloon -> bot/room/admin tools too */
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
                    vc.llm.context_tokens   = app.llm_context_tokens > 0 ? app.llm_context_tokens : app.llm_context_detected;
                    vc.llm.compact_pct      = app.llm_compact_pct;
                    vc.llm.compact_strategy = app.llm_compact_strategy;
                    vc.llm.max_rounds       = app.llm_max_rounds;
                    vc.vision = cc_vision;
                    vc.max_ms     = opt->listen_max_sec > 0 ? opt->listen_max_sec * 1000 : 0;  /* 0 => 5 min */
                    vc.confirm_ms = opt->confirm_sec   > 0 ? opt->confirm_sec   * 1000 : 0;  /* 0 => 1 min */
                    /* Owner balloon: role-adaptive prompt, full owner toolset (incl. agentic coding).
                     * No wake word and no persona: the balloon is functional, not conversational — the
                     * owner talking to their own machine, push-to-talk, never addressed by name. Both
                     * belong to the fullbot plugin, for the room. */
                    bsdr_botprompt_build(vc.system_prompt, sizeof vc.system_prompt, BSDR_TG_ALL,
                                         BSDR_ACL_OWNER, "the owner", "", "", cc_vision, /*spoken=*/false);
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
            bsdr_app_unpair_now(&app);   /* DELIBERATE disconnect → stop the relay now (no grace) */
            teardown_session(&a);
            BSDR_INFO("bsdr.agent", "disconnected from Quest %s (operator request)", dropped);
        }
        bsdr_app_unpair_grace_expired(&app);      /* finalize a held relay once its grace timer lapses */
        /* Warm-resume grace lapsed with no re-pair (not a room change) → finalize the LAN teardown now. */
        bsdr_mutex_lock(a.lock);
        int warm_expired = a.warm_until && bsdr_now_ms() > a.warm_until;
        if (warm_expired) a.warm_until = 0;
        bsdr_mutex_unlock(a.lock);
        if (warm_expired) { teardown_session(&a); if (a.app) bsdr_app_set_streaming(&app, false);
                            BSDR_INFO("bsdr.agent", "warm-resume grace lapsed — LAN session torn down"); }
        if (tick % 5 == 0)                        /* ~1 s: reconcile internet sharing promptly */
            bsdr_app_cloud_tick(&app);            /* start/stop the relay stream to match desired */
        if (++follow_ctr >= 75) {                 /* ~15 s: follow the operator between rooms */
            follow_ctr = 0;
            bsdr_app_bot_follow_tick(&app);
            bsdr_app_bot_token_tick(&app);        /* keep the bot token fresh (renews ~every 10 min) */
        }
        if (++tick >= 25) {                       /* ~5 s heartbeat-expiry check */
            tick = 0;
            if (bsdr_control_expire_stale(ctl)) {
                /* heartbeat lost: tear down the LAN session but HOLD the relay on the grace timer —
                 * a transient Wi-Fi gap / headset-off should not drop the internet share. */
                bsdr_app_set_paired(&app, false, NULL, NULL);
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
    if (app.roomcmd) {   /* stop the room command router before the injector it borrows goes away */
        bsdr_roomcmd *rcmd = (bsdr_roomcmd *)app.roomcmd;
        bsdr_mutex_lock(app.lock); app.roomcmd = NULL; bsdr_mutex_unlock(app.lock);
        bsdr_roomcmd_free(rcmd);
    }
    if (a.voice) bsdr_voice_free(a.voice);
    if (voice_inj) bsdr_injector_destroy(voice_inj);   /* voice borrows it; we own it */
    if (a.overlay) bsdr_overlay_free(a.overlay);
    app.update_stop = 1;   /* let the detached update checker exit its sleep loop */
    if (appwin) bsdr_appwindow_stop(appwin);   /* remove tray, close the window, join its thread */
    if (ui) bsdr_webui_stop(ui);   /* stop serving before unloading plugins (no more http hook calls) */
    bsdr_plugins_unload();
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
#if defined(_WIN32)
#include <windows.h>
/* The Windows build is a GUI-subsystem app (-mwindows) so double-clicking it opens NO console window.
 * But we still want logs when the user runs it from a terminal or asks for them: attach to the parent's
 * console if we were launched from cmd/PowerShell, else pop one only when --console/--debug was passed.
 * Launched from Explorer with neither, stay windowless. Call this before anything writes to stdout. */
static void win_console_setup(int argc, char **argv) {
    int want = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--console") == 0 || strcmp(argv[i], "--debug") == 0) { want = 1; break; }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (want && AllocConsole())) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$",  "r", stdin);
    }
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
    win_console_setup(argc, argv);
#endif
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
        else if (strcmp(argv[i], "--unblank") == 0) {
            /* One-shot recovery: a prior crash left the monitor gamma-blanked. Restore and exit. */
            bsdr_screen_blank(0); bsdr_screen_blank_reset();
            printf("bsdrX: restored monitor gamma (unblanked)\n");
            return 0;
        }
        else if (strcmp(argv[i], "--debug") == 0) bsdr_log_set_level(BSDR_LOG_DEBUG);
        else if (strcmp(argv[i], "--insecure-tls") == 0) bsdr_tls_set_insecure(true);
        else if (strcmp(argv[i], "--control-only") == 0) opt.control_only = true;
        else if (strcmp(argv[i], "--no-video") == 0) opt.video = false;
        else if (strcmp(argv[i], "--no-audio") == 0) opt.audio = false;
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) opt.video_file = argv[++i];
        else if (strcmp(argv[i], "--file-gpu") == 0) opt.file_gpu = true;
        else if (strncmp(argv[i], "--terminal=", 11) == 0) opt.terminal = argv[i] + 11;   /* --terminal=pty|xvfb */
        else if (strcmp(argv[i], "--terminal") == 0) opt.terminal = "pty";               /* bare = headless-native pty */
        /* --virtual-desktop[=session-cmd]: xvfb backend in full-desktop mode. Bare = auto (the box's
         * default X session, else a lightweight WM + xterm); =CMD runs CMD as the whole session. */
        else if (strncmp(argv[i], "--virtual-desktop=", 18) == 0) { opt.terminal = "xvfb-desktop"; opt.terminal_cmd = argv[i] + 18; }
        else if (strcmp(argv[i], "--virtual-desktop") == 0) opt.terminal = "xvfb-desktop";
        else if (strcmp(argv[i], "--terminal-cmd") == 0 && i + 1 < argc) opt.terminal_cmd = argv[++i];
        else if (strcmp(argv[i], "--terminal-size") == 0 && i + 1 < argc) { int c=0,r=0; if (sscanf(argv[++i], "%dx%d", &c, &r) == 2) { opt.terminal_cols = c; opt.terminal_rows = r; } }
        else if (strcmp(argv[i], "--threed") == 0 && i + 1 < argc) opt.threed_mode = bsdr_threed_mode_parse(argv[++i]);
        else if (strcmp(argv[i], "--threed-deepness") == 0 && i + 1 < argc) opt.threed_deepness = atoi(argv[++i]);
        else if (strcmp(argv[i], "--threed-convergence") == 0 && i + 1 < argc) opt.threed_convergence = atoi(argv[++i]);
        else if (strcmp(argv[i], "--threed-swap") == 0) opt.threed_swap = 1;
        else if (strcmp(argv[i], "--threed-full") == 0) opt.threed_full = 1;
        else if (strcmp(argv[i], "--threed-ai") == 0 && i + 1 < argc) { opt.threed_ai_cmd = argv[++i]; if (!opt.threed_mode) opt.threed_mode = BSDR_3D_AI; }
        else if (strcmp(argv[i], "--threed-tier") == 0 && i + 1 < argc) { opt.threed_tier = (int)bsdr_depth_tier_parse(argv[++i]); if (!opt.threed_mode) opt.threed_mode = BSDR_3D_AI; }
        else if (strcmp(argv[i], "--faceswap-detect-every") == 0 && i + 1 < argc) { opt.faceswap_detect_every = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--x264-threads") == 0 && i + 1 < argc) { opt.x264_threads = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--ort-arena-off") == 0) { opt.ort_arena_off = true; }
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
        else if (strcmp(argv[i], "--web-port") == 0 && i + 1 < argc) opt.webui_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--web-bind") == 0 && i + 1 < argc) opt.webui_bind = argv[++i];
        else if (strcmp(argv[i], "--web-allow") == 0 && i + 1 < argc) opt.webui_allow = argv[++i];
        else if (strcmp(argv[i], "--no-ui") == 0) opt.webui_port = 0;
        else if (strcmp(argv[i], "--no-browser") == 0) opt.open_browser = false;
        else if (strcmp(argv[i], "--console") == 0) { /* Windows: force a console (see win_console_setup) */ }
        else if (strcmp(argv[i], "--browser") == 0 || strcmp(argv[i], "--no-app-window") == 0)
            opt.plain_browser = true;   /* open the default browser instead of the chromeless app window */
        else if (strcmp(argv[i], "--no-cloud-video") == 0) opt.no_cloud_video = true;
        else if (strcmp(argv[i], "--video-decoupled") == 0) opt.video_decoupled = true;
        else if (strcmp(argv[i], "--cpu") == 0) opt.cpu_only = true;
        else if (strcmp(argv[i], "--gpu") == 0) opt.gpu_encode = true;
        else if (strcmp(argv[i], "--lan-1x") == 0) opt.lan_1x = true;
        else if (strcmp(argv[i], "--vaapi") == 0) opt.use_vaapi = true;
        else if (strcmp(argv[i], "--kmsgrab") == 0) opt.use_kmsgrab = true;
        else if (strcmp(argv[i], "--x11") == 0) opt.force_x11 = true;
        else if (strcmp(argv[i], "--wayland") == 0 || strcmp(argv[i], "--pipewire") == 0) opt.force_pipewire = true;
        else if (strcmp(argv[i], "--pw-dmabuf") == 0) opt.pw_dmabuf = true;
        else if (strcmp(argv[i], "--cloud-rtcp-pli") == 0) opt.cloud_rtcp_pli = true;
        else if (strcmp(argv[i], "--sendmmsg") == 0) opt.use_sendmmsg = true;
        else if (strcmp(argv[i], "--no-sendmmsg") == 0) opt.no_sendmmsg = true;
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
        else if (strcmp(argv[i], "--bot-mode") == 0 && i + 1 < argc) opt.bot_mode = argv[++i];
        else if (strcmp(argv[i], "--encoder-mode") == 0 && i + 1 < argc) opt.encoder_mode = argv[++i];
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
"  --terminal[=BACKEND] Stream a shell/terminal to the headset (great on a HEADLESS box) with the\n"
"                       Quest's keyboard+mouse injected. BACKEND = pty (default) or xvfb:\n"
"                         pty  = in-process terminal (libvterm) rendered straight to video — NO X\n"
"                                needed; keystrokes go to the pty, mouse when the app enables it.\n"
"                         xvfb = a private Xvfb + xterm captured via x11grab, injected via XTEST\n"
"                                (full graphical terminal + mouse; needs Xvfb + xterm installed).\n"
"  --virtual-desktop[=CMD]\n"
"                       Like --terminal=xvfb but a FULL virtual desktop on the private Xvfb, not a\n"
"                       bare terminal — great for a truly headless box (no monitor/Xorg). With no\n"
"                       CMD it launches the machine's configured default X session (GNOME/KDE/XFCE/\n"
"                       ...), or, if none, a lightweight WM (openbox/fluxbox/icewm/...) + an xterm.\n"
"                       =CMD runs CMD as the whole session (e.g. --virtual-desktop=startxfce4).\n"
"                       Needs Xvfb + xterm (+ a WM/desktop) installed.\n"
"  --terminal-cmd CMD   Program to run in the terminal (default: $SHELL, else /bin/bash).\n"
"  --terminal-size CxR  pty grid size in columns x rows (default 120x36).\n"
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
"  --faceswap-detect-every N  Run face DETECTION only every N frames (swap still every frame);\n"
"                       ~halves faceswap CPU at a small tracking-lag cost. Default 1 (every frame).\n"
"  --x264-threads N     Software (--cpu) encode: use N x264 FRAME threads (still one NAL/frame) to\n"
"                       spread encode across cores. Adds ~(N-1) frames latency; default 1 (off).\n"
"  --ort-arena-off      EXPERIMENTAL: disable ORT's CPU memory arena on the depth/faceswap models\n"
"                       (lowers steady RSS while they're idle; output unchanged).\n"
"  --no-video           Don't stream video (input/control only).\n"
"  --no-audio           Don't stream desktop audio, expose the headset mic, or bring up the\n"
"                       full-bot's cloud room-mic producer.\n"
"\n"
"HEADSET\n"
"  --quest_ip IP        Only pair with this headset; ignore all others.\n"
"  --control-only       Run discovery + control + web UI, but never stream.\n"
"\n"
"OWNER MIC (intercept the headset owner's voice)\n"
"  --sniff-mic          Sniff the Quest's room mic (plain Opus RTP to the cloud, which\n"
"                       it never sends us) and expose it as the BSDR_QuestMic\n"
"                       virtual microphone (the headset owner's voice).\n"
"                       Uses --quest_ip if set, else the paired headset. The privileged\n"
"                       capture runs in a helper started via sudo (prompts on the terminal);\n"
"                       can also be toggled live from the web panel. Also runnable as root.\n"
"  --sniff-mitm         Also ARP-spoof Quest<->gateway so a switched LAN routes the\n"
"                       mic through us (toggles ip_forward; heals ARP on exit). Implies --sniff-mic.\n"
"  --sniff-iface IF     Capture interface (default: the default-route interface).\n"
"  --sniff-gw IP        Gateway IP for --sniff-mitm (default: the default-route gateway).\n"
"  --sniff-remote PORT  Receive the owner mic from a router companion (bsdr_micrelay) instead of\n"
"                       capturing locally. Works over WiFi (the router sees the headset's traffic);\n"
"                       no root/MITM. Omit or set 45099 to use AUTO-DISCOVERY: just run\n"
"                       'bsdr_micrelay --iface br-lan' on the router — it beacons, this agent finds\n"
"                       it and registers for the headset it's paired with (the relay only forwards a\n"
"                       headset's mic to the agent it observed paired with it), and one relay serves\n"
"                       many agents/headsets at once. No IPs/ports to hand-configure.\n"
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
"  --web-port N         Port for the local control panel (default 8088).\n"
"  --web-bind ADDR      Control-panel listen address (default 127.0.0.1). Use 0.0.0.0 for all\n"
"                       interfaces or a specific IP to reach the panel from another device.\n"
"                       Off-loopback is UNAUTHENTICATED — firewall it or put it behind a proxy.\n"
"  --web-allow HOSTS    Extra Host/Origin values the CSRF guard accepts (comma-separated): a LAN\n"
"                       IP, or your reverse-proxy hostname. '*' accepts any (behind an auth proxy).\n"
"  --no-ui              Disable the web control panel entirely (no server).\n"
"  --no-browser         Run the panel but don't auto-open it (reach it manually).\n"
"  --browser            Open the panel in your default browser instead of the native app window\n"
"                       (the app window is the default on Windows, Linux and macOS).\n"
"  --console            Windows: force a console window for logs (the GUI build has none by default;\n"
"                       running from a terminal already shows logs there).\n"
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
"                           ephemeral source ports per relay IP across share toggles AND\n"
"                           across a process restart (persisted in the config dir), like\n"
"                           the official client, so the relay's comedia latch stays valid;\n"
"                           this flag reverts to fresh ephemeral each time.\n"
"  --encoder-mode MODE      'quality' (default), 'balanced', or 'performance'. Higher levels use\n"
"                           a lighter encoder preset (quality NVENC p7 + 2-pass; balanced p6\n"
"                           single-pass / x264 faster; performance p4 single-pass / x264\n"
"                           superfast) for less CPU/GPU cost at a small quality drop.\n"
"                           Persisted; the web UI has the same toggle. Applies on\n"
"                           the next stream (re)start.\n"
"  --bot-mode MODE          Second-account bot presence. 'audio' (default) is the bare\n"
"                           built-in: REST-join the host room to unlock the owner mic when\n"
"                           alone; no avatar, no assistant. Other modes are provided by\n"
"                           plugins — the 'fullbot' plugin adds 'fullbot' (avatar + LLM\n"
"                           assistant/moderation); legacy 'full' maps to it. Persisted; the\n"
"                           web UI Presence dropdown lists whatever is available.\n"
"\n"
"PERFORMANCE\n"
"  --cpu                    Force CPU scale + libx264 encode. This is the DEFAULT on\n"
"                           Linux: x264 keeps low-bitrate text sharper than NVENC (which\n"
"                           lacks psychovisual RD). Also enables the overlay/faceswap path.\n"
"  --gpu                    Opt into the CUDA/NVENC pipeline (hwupload+scale_cuda+nvenc)\n"
"                           for GPU offload / high bitrate / game content. Default on\n"
"                           Windows/macOS. Saved settings and this flag persist across\n"
"                           restarts; an explicit --cpu/--gpu on the command line wins.\n"
"  --lan-1x                 Send LAN video once instead of 2x — halves the uplink\n"
"                           on a weak WiFi link (default: 2x, like the official host).\n"
"  --vaapi                  Encode on the iGPU via VAAPI instead of NVENC (frees the\n"
"                           dGPU; AMD needs mesa radeonsi). Falls back to CPU on failure.\n"
"  --kmsgrab                Capture via DRM/KMS instead of x11grab (zero-copy with\n"
"                           --vaapi). Needs CAP_SYS_ADMIN: setcap cap_sys_admin+ep build/bsdr_agent.\n"
"  --pw-dmabuf              EXPERIMENTAL: negotiate dmabuf from PipeWire (Wayland) and import it\n"
"                           zero-copy into VAAPI. Needs --vaapi + a Wayland session; falls back to\n"
"                           the CPU path if dmabuf negotiation or the VAAPI import fails.\n"
"  --sendmmsg               Batch each NAL's UDP fragments into one syscall (Linux; ON by default).\n"
"  --no-sendmmsg            Disable the batched send; one sendto per datagram (wire output identical).\n"
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
        /* Anything else that still looks like a flag is a typo or a renamed option (--ui-port is now
         * --web-port). Say so: silently ignoring it starts the agent on the defaults instead, which
         * reads exactly like the flag was honoured — you only notice when the panel isn't on the port
         * you asked for. Not fatal, so a bad flag never takes down a working setup. A flag's VALUE is
         * consumed by its own branch above and never reaches here; a known flag whose value is missing
         * does, though (its branch needs i+1<argc), hence the two-part wording. */
        else if (argv[i][0] == '-' && argv[i][1])
            fprintf(stderr, "bsdr_agent: ignoring '%s' — unknown option, or a known one missing its "
                            "value (see --help)\n", argv[i]);
    }

    signal(SIGINT, on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, on_sigint);   /* `kill` should also clean up the virtual devices */
#endif
#ifdef SIGPIPE
    /* Never let a broken pipe kill the agent. The web/control servers write responses to sockets a
     * browser can close mid-send (navigate away, aborted fetch, the store OAuth poll) — send() then
     * raises SIGPIPE, whose default action is to TERMINATE. Ignore it so send() just returns EPIPE and
     * bsdr_send_all reports the error normally. (Not defined on Windows, where sockets never raise it.) */
    signal(SIGPIPE, SIG_IGN);
#endif
    /* Restore the monitor on a CRASH too (not just Ctrl-C): these terminate the process, but the blank
     * is persistent gamma state, so without this a segfault would leave the screen black. */
    { int fatal[] = {
#ifdef SIGSEGV
        SIGSEGV,
#endif
#ifdef SIGABRT
        SIGABRT,
#endif
#ifdef SIGBUS
        SIGBUS,
#endif
#ifdef SIGFPE
        SIGFPE,
#endif
#ifdef SIGILL
        SIGILL,
#endif
#ifdef SIGQUIT
        SIGQUIT,
#endif
#ifdef SIGHUP
        SIGHUP,
#endif
        0 };
      for (int i = 0; fatal[i]; i++) signal(fatal[i], on_fatal);
    }
    /* Self-heal: an uncatchable death in a PRIOR run (SIGKILL / power loss) can leave the monitor
     * gamma-blanked with no chance to restore. Clear any stuck blank once at startup so simply
     * relaunching bsdrX brings the screen back (X11/Win/macOS only; Wayland already self-heals). */
    bsdr_screen_blank_reset();
    atexit(bsdr_audio_cleanup_stale_devices);   /* backstop for any normal exit() path */
    int rc = bsdr_agent_run(&opt);
    bsdr_audio_cleanup_stale_devices();         /* belt-and-suspenders on clean shutdown */
    return rc;
}
#endif /* BSDR_NO_CLI_MAIN */
