/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Text-to-speech core — see bsdr/tts.h. Local (Piper CLI) + cloud (OpenAI-compatible /audio/speech),
 * both normalized to 48 kHz mono s16 PCM. */
#include "bsdr/tts.h"
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"
#include "minimp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#  include <unistd.h>
#endif

#define TTS_OUT_HZ 48000

/* ---- linear resample mono s16 src_hz -> 48 kHz. Returns malloc'd 48k buffer + frame count. ------- */
static int resample_48k_mono(const int16_t *in, int in_frames, int in_hz, int16_t **out) {
    if (in_frames <= 0 || in_hz <= 0) { *out = NULL; return 0; }
    if (in_hz == TTS_OUT_HZ) {
        int16_t *o = malloc((size_t)in_frames * 2);
        if (!o) return -1;
        memcpy(o, in, (size_t)in_frames * 2);
        *out = o; return in_frames;
    }
    /* out_frames = in_frames * 48000 / in_hz */
    long out_frames = (long)in_frames * TTS_OUT_HZ / in_hz;
    if (out_frames <= 0) { *out = NULL; return 0; }
    int16_t *o = malloc((size_t)out_frames * 2);
    if (!o) return -1;
    double step = (double)in_hz / TTS_OUT_HZ;
    for (long i = 0; i < out_frames; i++) {
        double sp = i * step;
        long i0 = (long)sp;
        double frac = sp - i0;
        long i1 = i0 + 1; if (i1 >= in_frames) i1 = in_frames - 1;
        double s = in[i0] * (1.0 - frac) + in[i1] * frac;
        o[i] = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
    }
    *out = o; return (int)out_frames;
}

/* Downmix interleaved s16 to mono in place-ish; returns mono frame count into `mono` (malloc'd). */
static int to_mono_s16(const uint8_t *pcm, int bytes, int channels, int16_t **mono) {
    if (channels < 1) channels = 1;
    int total = bytes / 2;                 /* total samples across channels */
    int frames = total / channels;
    int16_t *m = malloc((size_t)(frames > 0 ? frames : 1) * 2);
    if (!m) return -1;
    const int16_t *s = (const int16_t *)pcm;
    for (int f = 0; f < frames; f++) {
        int acc = 0;
        for (int c = 0; c < channels; c++) acc += s[f * channels + c];
        m[f] = (int16_t)(acc / channels);
    }
    *mono = m; return frames;
}

/* Parse a WAV (RIFF/WAVE) buffer: locate fmt (channels, rate, bits) + data chunk. Returns 0 on ok. */
static int parse_wav(const uint8_t *b, int len, int *hz, int *ch, const uint8_t **data, int *data_len) {
    if (len < 44 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) return -1;
    int pos = 12, rate = 0, chans = 1, bits = 16, dpos = -1, dlen = 0;
    while (pos + 8 <= len) {
        const uint8_t *id = b + pos;
        uint32_t sz = (uint32_t)b[pos+4] | ((uint32_t)b[pos+5]<<8) | ((uint32_t)b[pos+6]<<16) | ((uint32_t)b[pos+7]<<24);
        int body = pos + 8;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= len) {
            chans = b[body+2] | (b[body+3]<<8);
            rate  = b[body+4] | (b[body+5]<<8) | (b[body+6]<<16) | (b[body+7]<<24);
            bits  = b[body+14] | (b[body+15]<<8);
        } else if (memcmp(id, "data", 4) == 0) {
            dpos = body; dlen = (int)sz;
            if (dpos + dlen > len) dlen = len - dpos;   /* truncated: use what we have */
            break;
        }
        pos = body + (int)sz + ((int)sz & 1);           /* chunks are word-aligned */
    }
    if (dpos < 0 || rate <= 0 || bits != 16) return -1;
    *hz = rate; *ch = chans < 1 ? 1 : chans; *data = b + dpos; *data_len = dlen;
    return 0;
}

/* raw/whatever bytes (s16) at src_hz/channels -> 48k mono. Consumes pcm; sets *out (malloc'd). */
static int finalize_pcm(const uint8_t *pcm, int bytes, int src_hz, int channels, int16_t **out) {
    int16_t *mono = NULL;
    int frames = to_mono_s16(pcm, bytes, channels, &mono);
    if (frames < 0) return -1;
    int rc = resample_48k_mono(mono, frames, src_hz, out);
    free(mono);
    return rc;
}

/* Decode an MP3 byte buffer (minimp3) to 48 kHz mono. Returns frames or <=0. */
static int mp3_to_48k_mono(const uint8_t *mp3, int mp3_len, int16_t **out) {
    mp3dec_t dec; mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = 1u << 18, len = 0;                 /* interleaved s16 samples */
    int16_t *acc = malloc(cap * sizeof(int16_t));
    if (!acc) return -1;
    int hz = 0, ch = 1, pos = 0;
    while (pos < mp3_len) {
        int samples = mp3dec_decode_frame(&dec, mp3 + pos, mp3_len - pos, frame, &info);
        if (info.frame_bytes <= 0) break;           /* no more syncable frames */
        pos += info.frame_bytes;
        if (samples > 0) {
            hz = info.hz; ch = info.channels ? info.channels : 1;
            size_t n = (size_t)samples * ch;
            if (len + n > cap) { while (len + n > cap) cap *= 2; int16_t *nb = realloc(acc, cap * sizeof(int16_t)); if (!nb) break; acc = nb; }
            memcpy(acc + len, frame, n * sizeof(int16_t)); len += n;
        }
    }
    if (len == 0 || hz <= 0) { free(acc); return -1; }
    int rc = finalize_pcm((const uint8_t *)acc, (int)(len * sizeof(int16_t)), hz, ch, out);
    free(acc);
    return rc;
}

/* ---------------------------------------------------------------- freetts.org (custom API, no key) */
static int synth_freetts(const bsdr_tts_config *cfg, const char *text, int16_t **out) {
    char etext[8192]; bsdr_json_escape(etext, sizeof etext, text);
    const char *voice = cfg->voice[0] ? cfg->voice : "en-US-JennyNeural";
    char body[9000];
    int blen = snprintf(body, sizeof body,
        "{\"text\":\"%s\",\"voice\":\"%s\",\"rate\":\"+0%%\",\"pitch\":\"+0Hz\"}", etext, voice);
    char resp1[8192];
    int t1 = bsdr_http_request("POST", "https://freetts.org/api/tts", NULL, 0, "application/json",
                               body, (size_t)blen, resp1, sizeof resp1);
    if (t1 <= 0 || bsdr_http_status(resp1) != 200) {
        BSDR_WARN("bsdr.tts", "freetts /tts HTTP %d", t1 > 0 ? bsdr_http_status(resp1) : -1);
        return -1;
    }
    const char *b1 = bsdr_http_body(resp1); if (!b1) return -1;
    char fid[128] = ""; bsdr_json_get_str(b1, "file_id", fid, sizeof fid);
    /* fid goes into a URL — accept only [A-Za-z0-9-] so it can't inject path/host. */
    if (!fid[0]) { BSDR_WARN("bsdr.tts", "freetts: no file_id in response"); return -1; }
    for (const char *p = fid; *p; p++)
        if (!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-')) { BSDR_WARN("bsdr.tts", "freetts: bad file_id"); return -1; }
    char url[256]; snprintf(url, sizeof url, "https://freetts.org/api/audio/%s", fid);
    size_t cap = 8u * 1024 * 1024;
    char *resp2 = malloc(cap);
    if (!resp2) return -1;
    int rc = -1;
    for (int attempt = 0; attempt < 3; attempt++) {   /* the file may take a beat to be ready */
        int t2 = bsdr_http_request("GET", url, NULL, 0, NULL, NULL, 0, resp2, cap);
        int st = t2 > 0 ? bsdr_http_status(resp2) : -1;
        const char *b2 = t2 > 0 ? bsdr_http_body(resp2) : NULL;
        if (st == 200 && b2) { rc = mp3_to_48k_mono((const uint8_t *)b2, t2 - (int)(b2 - resp2), out); break; }
        bsdr_sleep_ms(300);
    }
    free(resp2);
    if (rc > 0) BSDR_INFO("bsdr.tts", "freetts: %d samples @48k (voice=%s)", rc, voice);
    else BSDR_WARN("bsdr.tts", "freetts: could not fetch/decode audio");
    return rc;
}

/* ------------------------------------------------------------------------- cloud (OpenAI-compatible) */
static int synth_cloud(const bsdr_tts_config *cfg, const char *text, int16_t **out) {
    if (!cfg->endpoint[0] || !cfg->token[0]) {
        BSDR_WARN("bsdr.tts", "cloud TTS needs an endpoint + API token (see the TTS card)");
        return -1;
    }
    char etext[8192]; bsdr_json_escape(etext, sizeof etext, text);
    const char *model = cfg->cloud_model[0] ? cfg->cloud_model : "tts-1";
    const char *voice = cfg->voice[0] ? cfg->voice : "alloy";
    char *body = malloc(9000);
    if (!body) return -1;
    int blen = snprintf(body, 9000,
        "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\",\"response_format\":\"wav\"}",
        model, etext, voice);
    char auth[300]; snprintf(auth, sizeof auth, "Bearer %s", cfg->token);
    bsdr_http_header hdrs[] = { { "Authorization", auth } };
    size_t cap = 8u * 1024 * 1024;                 /* up to ~8 MB of audio */
    char *resp = malloc(cap);
    if (!resp) { free(body); return -1; }
    int total = bsdr_http_request("POST", cfg->endpoint, hdrs, 1, "application/json",
                                  body, (size_t)blen, resp, cap);
    free(body);
    if (total <= 0) { BSDR_WARN("bsdr.tts", "cloud TTS request failed"); free(resp); return -1; }
    int status = bsdr_http_status(resp);
    const char *bodyp = bsdr_http_body(resp);
    if (status != 200 || !bodyp) {
        BSDR_WARN("bsdr.tts", "cloud TTS HTTP %d: %.180s", status, bodyp ? bodyp : "(no body)");
        free(resp); return -1;
    }
    int body_len = total - (int)(bodyp - resp);
    int rc;
    int hz = 0, ch = 1, dlen = 0; const uint8_t *data = NULL;
    if (parse_wav((const uint8_t *)bodyp, body_len, &hz, &ch, &data, &dlen) == 0) {
        rc = finalize_pcm(data, dlen, hz, ch, out);
    } else {
        /* Not a WAV — assume raw s16le mono 24 kHz (OpenAI "pcm" default) as a fallback. */
        BSDR_DEBUG("bsdr.tts", "cloud TTS: no WAV header, treating %d bytes as raw s16le 24k mono", body_len);
        rc = finalize_pcm((const uint8_t *)bodyp, body_len, 24000, 1, out);
    }
    free(resp);
    if (rc > 0) BSDR_INFO("bsdr.tts", "cloud TTS: %d samples @48k (model=%s voice=%s)", rc, model, voice);
    return rc;
}

/* ------------------------------------------------------------------------------------ local (Piper) */
/* Piper runs an external POSIX process (fork/exec) — the whole local path is compiled out on Windows
 * (synth_local stubs there), so these helpers are only defined where synth_local actually uses them. */
#if !defined(_WIN32)
static int has_quote(const char *s) { return s && strchr(s, '\'') != NULL; }

static int piper_model_rate(const char *model) {
    char cfgp[600]; snprintf(cfgp, sizeof cfgp, "%s.json", model);   /* voice.onnx -> voice.onnx.json */
    FILE *f = fopen(cfgp, "rb");
    if (!f) return 22050;
    char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    double d; if (bsdr_json_get_double(buf, "sample_rate", &d) && d > 0) return (int)d;
    return 22050;
}
#endif

static int synth_local(const bsdr_tts_config *cfg, const char *text, int16_t **out) {
#if defined(_WIN32)
    (void)cfg; (void)text; (void)out;
    BSDR_WARN("bsdr.tts", "local Piper TTS is POSIX-only on this build; use the cloud engine");
    return -1;
#else
    const char *piper = cfg->piper[0] ? cfg->piper : "piper";
    if (!cfg->model[0]) { BSDR_WARN("bsdr.tts", "local TTS needs a Piper voice model (--tts-model / UI)"); return -1; }
    if (has_quote(piper) || has_quote(cfg->model)) { BSDR_WARN("bsdr.tts", "TTS: quote in piper/model path rejected"); return -1; }
    /* text -> temp file (arbitrary content is safe as a file; avoids shell-escaping the sentence) */
    char tmpl[] = "/tmp/bsdr-tts-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { BSDR_WARN("bsdr.tts", "TTS: mkstemp failed"); return -1; }
    { size_t tl = strlen(text); if (write(fd, text, tl) != (ssize_t)tl) {} }
    close(fd);
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "'%s' --model '%s' --output_raw < '%s' 2>/dev/null", piper, cfg->model, tmpl);
    FILE *p = popen(cmd, "r");
    if (!p) { unlink(tmpl); BSDR_WARN("bsdr.tts", "TTS: cannot run '%s'", piper); return -1; }
    size_t cap = 1u << 20, len = 0;                 /* grow as needed */
    uint8_t *raw = malloc(cap);
    if (!raw) { pclose(p); unlink(tmpl); return -1; }
    size_t r;
    while ((r = fread(raw + len, 1, cap - len, p)) > 0) {
        len += r;
        if (len == cap) { size_t nc = cap * 2; uint8_t *nb = realloc(raw, nc); if (!nb) break; raw = nb; cap = nc; }
    }
    int status = pclose(p);
    unlink(tmpl);
    if (len < 2) { BSDR_WARN("bsdr.tts", "local TTS produced no audio (piper exit %d — model/path ok?)", status); free(raw); return -1; }
    int hz = piper_model_rate(cfg->model);
    int rc = finalize_pcm(raw, (int)len, hz, 1 /*piper is mono*/, out);
    free(raw);
    if (rc > 0) BSDR_INFO("bsdr.tts", "local TTS: %d samples @48k (piper %dHz)", rc, hz);
    return rc;
#endif
}

/* --------------------------------------------------------------------------------------------- api */
int bsdr_tts_synth(const bsdr_tts_config *cfg, const char *text, int16_t **pcm_out) {
    if (!cfg || !text || !text[0] || !pcm_out) return -1;
    *pcm_out = NULL;
    switch (cfg->engine) {
        case BSDR_TTS_CLOUD:   return synth_cloud(cfg, text, pcm_out);
        case BSDR_TTS_FREETTS: return synth_freetts(cfg, text, pcm_out);
        default:               return synth_local(cfg, text, pcm_out);
    }
}

int bsdr_tts_available(const bsdr_tts_config *cfg) {
    if (!cfg) return 0;
    if (cfg->engine == BSDR_TTS_FREETTS) return 1;                    /* public, no key */
    if (cfg->engine == BSDR_TTS_CLOUD) return cfg->endpoint[0] && cfg->token[0];
#if defined(_WIN32)
    return 0;
#else
    if (!cfg->model[0]) return 0;
    struct stat st; return stat(cfg->model, &st) == 0;    /* model present; piper resolved at run */
#endif
}
