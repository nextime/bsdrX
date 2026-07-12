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
/* Speech-to-text client: PCM -> WAV -> multipart POST -> {text}. */
#include "bsdr/stt.h"
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wr16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

/* Build a 16-bit PCM WAV (downmix to mono, downsample to 16 kHz for whisper). */
static size_t build_wav(const int16_t *pcm, int frames, int rate, int channels,
                        uint8_t *out, size_t cap) {
    int out_rate = 16000;
    int step = rate / out_rate; if (step < 1) step = 1;
    int out_frames = frames / step;
    size_t data_bytes = (size_t)out_frames * 2;            /* mono 16-bit */
    if (44 + data_bytes > cap) { out_frames = (int)((cap - 44) / 2); data_bytes = (size_t)out_frames * 2; }

    memcpy(out, "RIFF", 4);
    wr32(out + 4, (uint32_t)(36 + data_bytes));
    memcpy(out + 8, "WAVEfmt ", 8);
    wr32(out + 16, 16);            /* fmt chunk size */
    wr16(out + 20, 1);             /* PCM */
    wr16(out + 22, 1);             /* mono */
    wr32(out + 24, (uint32_t)out_rate);
    wr32(out + 28, (uint32_t)(out_rate * 2));
    wr16(out + 32, 2);             /* block align */
    wr16(out + 34, 16);            /* bits */
    memcpy(out + 36, "data", 4);
    wr32(out + 40, (uint32_t)data_bytes);

    uint8_t *d = out + 44;
    for (int i = 0; i < out_frames; i++) {
        int idx = (i * step) * channels;
        int s = pcm[idx];
        if (channels > 1) s = (s + pcm[idx + 1]) / 2;       /* downmix L+R */
        wr16(d + i * 2, (uint16_t)(int16_t)s);
    }
    return 44 + data_bytes;
}

bool bsdr_stt_transcribe(const bsdr_stt_config *cfg,
                         const int16_t *pcm, int frames, int rate, int channels,
                         char *out, size_t out_len) {
    /* No endpoint configured -> fall back to the built-in free online service. */
    const char *endpoint = cfg->endpoint[0] ? cfg->endpoint : BSDR_STT_FREE_ENDPOINT;
    bool hf = strstr(endpoint, "huggingface") != NULL;
    if (!cfg->endpoint[0]) {
        if (!cfg->token[0])
            BSDR_WARN("bsdr.stt", "no STT endpoint/token set — the free anonymous HuggingFace STT was "
                      "removed. Set an STT endpoint+token in the panel: a free-tier Groq "
                      "(https://api.groq.com/openai/v1/audio/transcriptions, model whisper-large-v3-turbo), "
                      "OpenAI, a HuggingFace token, or a self-hosted whisper.cpp on your LAN.");
        else
            BSDR_INFO("bsdr.stt", "no STT endpoint set -> HuggingFace router (%s) with your token", endpoint);
    }

    /* wav + body were process-lifetime statics (~8 MB) though STT runs rarely. malloc them per call
     * and free once the request is sent (the alloc is dwarfed by the network round-trip). */
    const size_t WAV_CAP = 4 * 1024 * 1024;          /* up to ~130 s mono@16k (matches capture ceiling) */
    const size_t BODY_CAP = 4 * 1024 * 1024 + 4096;
    uint8_t *wav = malloc(WAV_CAP);
    uint8_t *body = malloc(BODY_CAP);
    if (!wav || !body) { free(wav); free(body); BSDR_ERROR("bsdr.stt", "oom"); return false; }
    size_t wav_len = build_wav(pcm, frames, rate, channels, wav, WAV_CAP);

    size_t n = 0;
    char ctype[96];
    if (hf) {
        /* HuggingFace inference wants the raw audio body, not OpenAI multipart. */
        memcpy(body, wav, wav_len); n = wav_len;
        snprintf(ctype, sizeof(ctype), "audio/wav");
    } else {
        /* OpenAI-compatible multipart/form-data: model + file */
        const char *boundary = "----bsdrXSTTb0undary";
        const char *model = cfg->model[0] ? cfg->model : "whisper-1";
        n += snprintf((char *)body + n, BODY_CAP - n,
            "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
            "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
            "Content-Type: audio/wav\r\n\r\n", boundary, model, boundary);
        memcpy(body + n, wav, wav_len); n += wav_len;
        n += snprintf((char *)body + n, BODY_CAP - n, "\r\n--%s--\r\n", boundary);
        snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
    }
    char auth[320];
    bsdr_http_header hdrs[1];
    int nh = 0;
    if (cfg->token[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->token);
        hdrs[nh].name = "Authorization"; hdrs[nh].value = auth; nh++;
    }

    static char resp[65536];
    int r = bsdr_http_request("POST", endpoint, hdrs, nh, ctype, body, n,
                              resp, sizeof(resp));
    free(wav); free(body);                           /* done with the request payload */
    if (r < 0) { BSDR_ERROR("bsdr.stt", "request failed"); return false; }
    int status = bsdr_http_status(resp);
    const char *bdy = bsdr_http_body(resp);
    if (status / 100 != 2 || !bdy) {
        BSDR_ERROR("bsdr.stt", "HTTP %d from %s", status, endpoint);
        return false;
    }
    /* OpenAI + whisper.cpp both return {"text":"..."} */
    if (!bsdr_json_get_str(bdy, "text", out, out_len)) {
        BSDR_WARN("bsdr.stt", "no text field in response"); return false;
    }
    BSDR_INFO("bsdr.stt", "transcribed: %.60s", out);
    return true;
}
