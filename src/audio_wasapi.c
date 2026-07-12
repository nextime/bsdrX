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
/* Windows audio backend (WASAPI) — the platform half of audio.h, mirroring the
 * PulseAudio code in audio.c. Provides bsdr_pa I/O + bsdr_audio_devices.
 *
 * Desktop out: loopback-capture the default render endpoint -> our Opus sender.
 * Mic in: render the decoded Quest mic into the virtual device "BSRD_Mic"
 *   (the installer renames VB-CABLE's endpoints to BSRD_Mic; other apps then
 *   record from "BSRD_Mic" as a microphone). We match the render endpoint by
 *   friendly-name substring so the install-time rename is transparent here.
 *
 * Shared-mode, 48 kHz, S16. Render streams use AUTOCONVERTPCM so the engine
 * adapts our format. Loopback capture arrives in the endpoint mix format
 * (usually 32-bit float) and is converted to S16 here.
 *
 * NOTE: compile-validated under mingw; runtime tuning requires a real Windows
 * host (WASAPI can't run under wine). */
#ifdef _WIN32
#include "bsdr/audio.h"
#include "bsdr/protocol.h"
#include "bsdr/log.h"

#define COBJMACROS                /* C convenience macros: IFace_Method(this, ...) */
#include <initguid.h>             /* instantiate the COM GUIDs/PKEYs locally */
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef BSDR_MIC_DEVICE_NAME
#define BSDR_MIC_DEVICE_NAME "BSRD_Mic"   /* virtual mic friendly name (installer-set) */
#endif

/* 100-ns units; ~40 ms engine buffer. */
#define BSDR_HNS_BUFFER 400000

struct bsdr_pa {
    IAudioClient        *client;
    IAudioCaptureClient *cap;     /* record/loopback */
    IAudioRenderClient  *ren;     /* playback */
    int channels;                 /* requested target channels (our PCM) */
    /* source mix format (loopback) */
    int src_channels, src_rate;
    bool src_float;               /* mix is 32-bit float (vs S16) */
    /* leftover capture samples (interleaved S16, target channels) */
    int16_t acc[BSDR_OPUS_FRAME * 8];
    int acc_count;                /* samples in acc */
    bool started;
    bool com_init;
};

static bool ensure_com(struct bsdr_pa *pa) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    pa->com_init = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    return pa->com_init;
}

static IMMDeviceEnumerator *make_enum(void) {
    IMMDeviceEnumerator *e = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void **)&e);
    return SUCCEEDED(hr) ? e : NULL;
}

/* Find a render endpoint whose friendly name contains `want` (ASCII). */
static IMMDevice *find_render_by_name(IMMDeviceEnumerator *en, const char *want) {
    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)))
        return NULL;
    UINT n = 0; IMMDeviceCollection_GetCount(col, &n);
    IMMDevice *found = NULL;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &dev))) continue;
        IPropertyStore *ps = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.pwszVal) {
                char name[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name) - 1, NULL, NULL);
                if (strstr(name, want)) { found = dev; IMMDevice_AddRef(found); }
            }
            PropVariantClear(&v);
            IPropertyStore_Release(ps);
        }
        IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(col);
    return found;
}

/* ---- open: render to a named device, or loopback the default render ---- */
static bsdr_pa *pa_open(const char *dev, int channels, bool record) {
    bsdr_pa *pa = calloc(1, sizeof(*pa));
    if (!pa) return NULL;
    pa->channels = channels;
    if (!ensure_com(pa)) { free(pa); return NULL; }

    IMMDeviceEnumerator *en = make_enum();
    if (!en) { BSDR_ERROR("bsdr.audio", "WASAPI: no device enumerator"); goto fail; }

    IMMDevice *mmdev = NULL;
    if (record) {
        /* desktop audio = loopback of the default render endpoint */
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &mmdev)))
            { BSDR_ERROR("bsdr.audio", "WASAPI: no default render endpoint"); goto fail_en; }
    } else {
        /* mic playback target: the named virtual device (BSRD_Mic / CABLE Input) */
        mmdev = (dev && *dev) ? find_render_by_name(en, dev) : NULL;
        if (!mmdev && FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &mmdev)))
            { BSDR_ERROR("bsdr.audio", "WASAPI: render device '%s' not found", dev ? dev : "(default)"); goto fail_en; }
    }

    if (FAILED(IMMDevice_Activate(mmdev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pa->client))) {
        BSDR_ERROR("bsdr.audio", "WASAPI: IAudioClient activate failed"); goto fail_dev;
    }

    HRESULT hr;
    if (record) {
        WAVEFORMATEX *mix = NULL;
        if (FAILED(IAudioClient_GetMixFormat(pa->client, &mix))) goto fail_dev;
        pa->src_channels = mix->nChannels;
        pa->src_rate     = (int)mix->nSamplesPerSec;
        pa->src_float    = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                           (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32);
        if (pa->src_rate != BSDR_AUDIO_CLOCK_HZ)
            BSDR_WARN("bsdr.audio", "WASAPI loopback at %d Hz (expected %d); no resample",
                      pa->src_rate, BSDR_AUDIO_CLOCK_HZ);
        hr = IAudioClient_Initialize(pa->client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK, BSDR_HNS_BUFFER, 0, mix, NULL);
        CoTaskMemFree(mix);
        if (FAILED(hr)) { BSDR_ERROR("bsdr.audio", "WASAPI loopback init 0x%lx", hr); goto fail_dev; }
        if (FAILED(IAudioClient_GetService(pa->client, &IID_IAudioCaptureClient, (void **)&pa->cap)))
            goto fail_dev;
    } else {
        WAVEFORMATEX wfx;
        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = (WORD)channels;
        wfx.nSamplesPerSec  = BSDR_AUDIO_CLOCK_HZ;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = (WORD)(channels * 2);
        wfx.nAvgBytesPerSec = BSDR_AUDIO_CLOCK_HZ * wfx.nBlockAlign;
        hr = IAudioClient_Initialize(pa->client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     BSDR_HNS_BUFFER, 0, &wfx, NULL);
        if (FAILED(hr)) { BSDR_ERROR("bsdr.audio", "WASAPI render init 0x%lx", hr); goto fail_dev; }
        if (FAILED(IAudioClient_GetService(pa->client, &IID_IAudioRenderClient, (void **)&pa->ren)))
            goto fail_dev;
    }
    IMMDevice_Release(mmdev);
    IMMDeviceEnumerator_Release(en);
    IAudioClient_Start(pa->client);
    pa->started = true;
    return pa;

fail_dev: if (mmdev) IMMDevice_Release(mmdev);
fail_en:  IMMDeviceEnumerator_Release(en);
fail:     bsdr_pa_close(pa);
          return NULL;
}

bsdr_pa *bsdr_pa_record_open(const char *source, int channels) {
    (void)source;   /* desktop loopback; source name is informational on Windows */
    return pa_open(source, channels, true);
}
bsdr_pa *bsdr_pa_play_open(const char *sink, int channels) {
    return pa_open(sink, channels, false);
}
/* No noisy backend error to suppress here (the loopback device is persistent) -> same as _play_open. */
bsdr_pa *bsdr_pa_play_open_quiet(const char *sink, int channels) {
    return pa_open(sink, channels, false);
}

/* Convert one captured packet (mix format) -> interleaved S16 at target channels. */
static void push_converted(bsdr_pa *pa, const BYTE *data, UINT32 frames) {
    int sc = pa->src_channels, tc = pa->channels;
    for (UINT32 f = 0; f < frames; f++) {
        int16_t s[8];
        for (int c = 0; c < tc; c++) {
            int src_c = c < sc ? c : sc - 1;   /* duplicate/limit channels */
            int32_t v;
            if (pa->src_float) {
                const float *fp = (const float *)data;
                float x = fp[f * sc + src_c];
                if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                v = (int32_t)(x * 32767.0f);
            } else {
                const int16_t *ip = (const int16_t *)data;
                v = ip[f * sc + src_c];
            }
            s[c] = (int16_t)v;
        }
        for (int c = 0; c < tc; c++) {
            if (pa->acc_count < (int)(sizeof(pa->acc) / sizeof(pa->acc[0])))
                pa->acc[pa->acc_count++] = s[c];
        }
    }
}

int bsdr_pa_read(bsdr_pa *pa, int16_t *pcm, int frames) {
    int need = frames * pa->channels;
    /* fill the accumulator until we have a full request */
    while (pa->acc_count < need) {
        UINT32 pkt = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(pa->cap, &pkt))) return -1;
        if (pkt == 0) { Sleep(2); continue; }
        BYTE *data; UINT32 nframes; DWORD flags;
        if (FAILED(IAudioCaptureClient_GetBuffer(pa->cap, &data, &nframes, &flags, NULL, NULL)))
            return -1;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            int add = (int)nframes * pa->channels;
            for (int i = 0; i < add && pa->acc_count < (int)(sizeof(pa->acc)/2); i++) pa->acc[pa->acc_count++] = 0;
        } else {
            push_converted(pa, data, nframes);
        }
        IAudioCaptureClient_ReleaseBuffer(pa->cap, nframes);
    }
    memcpy(pcm, pa->acc, (size_t)need * sizeof(int16_t));
    pa->acc_count -= need;
    if (pa->acc_count > 0) memmove(pa->acc, pa->acc + need, (size_t)pa->acc_count * sizeof(int16_t));
    return frames;
}

int bsdr_pa_write(bsdr_pa *pa, const int16_t *pcm, int frames) {
    UINT32 bufframes = 0, pad = 0;
    if (FAILED(IAudioClient_GetBufferSize(pa->client, &bufframes))) return -1;
    int written = 0;
    while (written < frames) {
        if (FAILED(IAudioClient_GetCurrentPadding(pa->client, &pad))) return -1;
        UINT32 avail = bufframes - pad;
        if (avail == 0) { Sleep(2); continue; }
        UINT32 n = (UINT32)(frames - written);
        if (n > avail) n = avail;
        BYTE *out;
        if (FAILED(IAudioRenderClient_GetBuffer(pa->ren, n, &out))) return -1;
        memcpy(out, pcm + (size_t)written * pa->channels, (size_t)n * pa->channels * sizeof(int16_t));
        IAudioRenderClient_ReleaseBuffer(pa->ren, n, 0);
        written += (int)n;
    }
    return frames;
}

void bsdr_pa_close(bsdr_pa *pa) {
    if (!pa) return;
    if (pa->started && pa->client) IAudioClient_Stop(pa->client);
    if (pa->cap) IAudioCaptureClient_Release(pa->cap);
    if (pa->ren) IAudioRenderClient_Release(pa->ren);
    if (pa->client) IAudioClient_Release(pa->client);
    if (pa->com_init) CoUninitialize();
    free(pa);
}

/* ---- virtual devices: no module loading on Windows; VB-CABLE provides the
 * endpoints and the installer renames them to BSRD_Mic. We just record desktop
 * loopback and render the mic into BSRD_Mic. ---- */
bool bsdr_audio_devices_create(bsdr_audio_devices *d) {
    memset(d, 0, sizeof(*d));
    d->speaker_module = d->mic_sink_module = d->mic_source_module = -1;
    /* desktop audio: loopback the default render endpoint (informational name) */
    snprintf(d->monitor_source, sizeof(d->monitor_source), "loopback:default-render");
    /* mic: render into the BSRD_Mic virtual device (VB-CABLE, installer-renamed) */
    snprintf(d->mic_sink, sizeof(d->mic_sink), "%s", BSDR_MIC_DEVICE_NAME);

    /* sanity: warn if the virtual mic isn't present yet (VB-CABLE not installed) */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool co = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    IMMDeviceEnumerator *en = make_enum();
    if (en) {
        IMMDevice *m = find_render_by_name(en, BSDR_MIC_DEVICE_NAME);
        if (m) IMMDevice_Release(m);
        else BSDR_WARN("bsdr.audio", "virtual mic '%s' not found — install VB-CABLE (the installer does this)",
                       BSDR_MIC_DEVICE_NAME);
        IMMDeviceEnumerator_Release(en);
    }
    if (co) CoUninitialize();

    d->active = true;
    BSDR_INFO("bsdr.audio", "WASAPI devices: desktop loopback + mic -> %s", d->mic_sink);
    return true;
}

void bsdr_audio_devices_destroy(bsdr_audio_devices *d) {
    if (!d->active) return;
    d->active = false;   /* nothing to unload on Windows */
}

void bsdr_audio_cleanup_stale_devices(void) { /* no virtual modules on Windows */ }

/* Standalone virtual mic for the owner-mic sniffer. On Windows the virtual mic is VB-CABLE
 * (user-installed, renamed BSRD_Mic by the installer) — we don't create it, just verify it's
 * present and clearly say so if not. The sniffer's player then renders into it via WASAPI. */
bool bsdr_virtual_mic_create(const char *sink_name, const char *source_name,
                             const char *description, int *sink_module, int *source_module) {
    (void)sink_name; (void)source_name;
    if (sink_module) *sink_module = -1;
    if (source_module) *source_module = -1;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool co = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    bool ok = false;
    IMMDeviceEnumerator *en = make_enum();
    if (en) {
        IMMDevice *m = find_render_by_name(en, BSDR_MIC_DEVICE_NAME);
        if (m) { IMMDevice_Release(m); ok = true; }
        IMMDeviceEnumerator_Release(en);
    }
    if (co) CoUninitialize();
    if (!ok)
        BSDR_ERROR("bsdr.audio", "%s needs VB-CABLE installed (https://vb-audio.com/Cable/) — the "
                   "installer renames its endpoints to '%s'. The decoded voice is rendered there; "
                   "apps then record it as that microphone.", description, BSDR_MIC_DEVICE_NAME);
    else
        BSDR_INFO("bsdr.audio", "virtual mic: routing %s into VB-CABLE (%s)", description, BSDR_MIC_DEVICE_NAME);
    return ok;
}
void bsdr_virtual_mic_destroy(int sink_module, int source_module) { (void)sink_module; (void)source_module; }

#endif /* _WIN32 */
