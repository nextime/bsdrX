/*
 * bsdrX plugin: faceswap — realtime face swap / deepfake onto the streamed video (ONNX).
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>.
 *
 * This faceswap plugin is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version. See the LICENSE file in this directory.
 *
 * Detached from the core: this plugin bundles the face-swap engine (faceswap.c — SCRFD detect +
 * ArcFace embed + inswapper_128) and registers it as a host SAME-DIMENSIONS VIDEO EFFECT
 * (host->video_fx_register, order 10 so it runs BEFORE 2D->3D). While loaded, the core routes each
 * encoder frame through it. The insightface MODELS come from the core's shared model store + its
 * model-manager UI (they stay in the core, shared with depth/voice); this plugin owns only the ONNX
 * PROCESSING. The effect config (enable / tier / source image / detect cadence) is the core's, read
 * back via host->faceswap_config. The source face is decoded via host->decode_image_rgb. Covered by
 * the bsdrX Plugin Exception. See PLAN-media-plugins.md (M-faceswap).
 */
#include "bsdr/plugin.h"
#include "bsdr/faceswap.h"  /* the bundled engine (compiled into this plugin) */
#include "bsdr/log.h"       /* bsdr_log_level; we PROVIDE bsdr_log() below so the engine's macros work */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- log shim: the bundled faceswap.c calls bsdr_log via BSDR_* macros; forward to the host. */
static const bsdr_plugin_host *g_host;
void bsdr_log(bsdr_log_level level, const char *tag, const char *fmt, ...) {
    if (!g_host || !g_host->log) return;
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    g_host->log((int)level, tag ? tag : "bsdr.plugin.faceswap", "%s", msg);
}
void bsdr_log_set_level(bsdr_log_level level) { (void)level; }   /* engine may reference it */

/* The bundled faceswap.c references this core global (--ort-arena-off, an experimental P4.6 flag).
 * Plugins can't see core symbols (RTLD_LOCAL), so provide our own — the arena stays on (the default);
 * the experimental flag simply doesn't reach the plugin. */
int bsdr_ort_arena_off = 0;

typedef struct {
    const bsdr_plugin_host *host;
    bsdr_faceswap *fs;
    int   on, tier, detect_every;     /* last-applied config */
    char  source[512];                /* last-applied source-image path */
    int   have_source;                /* a face identity is set on the engine */
    int   source_explicit;            /* source was set via the RGB interface (Android) — don't let the
                                       * config-path source clobber it */
    uint8_t *rgb; int rgb_w, rgb_h;   /* cached NV12<->RGB scratch (packed R,G,B), reallocated on resize */
    unsigned tick;
} fs_state;

/* ---- NV12 <-> packed RGB (integer BT.601 limited-range; fwd∘inv ≈ identity so a face-frame round-trip
 * doesn't tint the whole image). Only same-dims; the engine works in RGB. -------------------------- */
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static void nv12_to_rgb(const uint8_t *y, int ys, const uint8_t *uv, int uvs, int w, int h, uint8_t *rgb) {
    for (int j = 0; j < h; j++) {
        const uint8_t *yr = y + (size_t)j * ys;
        const uint8_t *ur = uv + (size_t)(j >> 1) * uvs;
        uint8_t *o = rgb + (size_t)j * w * 3;
        for (int i = 0; i < w; i++) {
            int c = yr[i] - 16;
            int d = ur[(i & ~1)]     - 128;   /* U */
            int e = ur[(i & ~1) + 1] - 128;   /* V */
            *o++ = clamp8((298 * c + 409 * e + 128) >> 8);
            *o++ = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            *o++ = clamp8((298 * c + 516 * d + 128) >> 8);
        }
    }
}

static void rgb_to_nv12(const uint8_t *rgb, int w, int h, uint8_t *y, int ys, uint8_t *uv, int uvs) {
    for (int j = 0; j < h; j++) {
        const uint8_t *ir = rgb + (size_t)j * w * 3;
        uint8_t *yo = y + (size_t)j * ys;
        for (int i = 0; i < w; i++) {
            int r = ir[i*3], g = ir[i*3+1], b = ir[i*3+2];
            yo[i] = clamp8(((66*r + 129*g + 25*b + 128) >> 8) + 16);
        }
    }
    /* UV: average each 2x2 block for a cleaner chroma downsample */
    for (int j = 0; j + 1 < h + 1; j += 2) {
        uint8_t *uo = uv + (size_t)(j >> 1) * uvs;
        for (int i = 0; i + 1 < w + 1; i += 2) {
            int rs = 0, gs = 0, bs = 0, n = 0;
            for (int dj = 0; dj < 2 && j + dj < h; dj++)
                for (int di = 0; di < 2 && i + di < w; di++) {
                    const uint8_t *p = rgb + ((size_t)(j+dj) * w + (i+di)) * 3;
                    rs += p[0]; gs += p[1]; bs += p[2]; n++;
                }
            if (!n) n = 1;
            int r = rs / n, g = gs / n, b = bs / n;
            uo[(i & ~1)]     = clamp8(((-38*r - 74*g + 112*b + 128) >> 8) + 128);   /* U */
            uo[(i & ~1) + 1] = clamp8((( 112*r - 94*g - 18*b + 128) >> 8) + 128);   /* V */
        }
    }
}

/* Open the engine for `tier` from the shared model store (<model_dir>/faceswap), reusing the current one
 * if the tier is unchanged. Returns 1 if the engine is ready. Reopen resets the source. */
static int fs_ensure_engine(fs_state *s, int tier) {
    const bsdr_plugin_host *h = s->host;
    if (s->fs && tier == s->tier) return 1;
    if (s->fs) { bsdr_faceswap_close(s->fs); s->fs = NULL; s->have_source = 0; s->source[0] = 0; s->source_explicit = 0; }
    char dir[600] = "", fsdir[700];
    if (h->model_dir) h->model_dir(dir, sizeof dir);
    snprintf(fsdir, sizeof fsdir, "%s%sfaceswap", dir, (dir[0] && dir[strlen(dir)-1] != '/') ? "/" : "");
    s->fs = bsdr_faceswap_open(fsdir, tier >= 2);
    s->tier = tier;
    return s->fs != NULL;
}

/* Re-read the core's face-swap config (throttled by the caller) and (re)load the engine / source. */
static void fs_refresh(fs_state *s) {
    const bsdr_plugin_host *h = s->host;
    int on = 0, tier = 0, det = 0; char src[512] = "";
    if (h->faceswap_config) h->faceswap_config(&on, &tier, src, sizeof src, &det);

    if (on && !fs_ensure_engine(s, tier)) { s->on = 0; return; }   /* models missing / ORT absent — retried */
    s->on = on;
    if (det != s->detect_every && s->fs) { bsdr_faceswap_set_detect_every(s->fs, det); s->detect_every = det; }

    /* (Re)set the source identity from the config PATH when it changes — unless the host set it directly
     * via the RGB interface (Android decodes the image itself), in which case that source wins. */
    if (on && s->fs && !s->source_explicit && strcmp(src, s->source) != 0) {
        s->have_source = 0;
        snprintf(s->source, sizeof s->source, "%s", src);
        if (src[0] && h->decode_image_rgb) {
            uint8_t *img = NULL; int iw = 0, ih = 0;
            if (h->decode_image_rgb(src, &img, &iw, &ih) == 0 && img) {
                s->have_source = (bsdr_faceswap_set_source_rgb(s->fs, img, iw, ih) == 0);
                free(img);
            }
        }
    }
}

/* Face-swap RGB interface (host-supplied packed RGB, e.g. Android GL): process a frame in place. */
static int fs_process_rgb(void *user, uint8_t *rgb, int w, int h) {
    fs_state *s = (fs_state *)user;
    if (w <= 0 || h <= 0) return -1;
    if ((s->tick++ % 30) == 0) fs_refresh(s);
    if (!s->on || !s->fs || !s->have_source || !bsdr_faceswap_ready(s->fs)) return 0;
    return bsdr_faceswap_process_rgb(s->fs, rgb, w, h);
}

/* Face-swap RGB interface: set the identity from a host-decoded packed-RGB source image. */
static int fs_set_source_rgb(void *user, const uint8_t *rgb, int w, int h) {
    fs_state *s = (fs_state *)user;
    if (w <= 0 || h <= 0) return -1;
    int on = 0, tier = 0, det = 0; char src[512] = "";
    if (s->host->faceswap_config) s->host->faceswap_config(&on, &tier, src, sizeof src, &det);
    if (!fs_ensure_engine(s, tier)) return -1;
    s->source_explicit = 1;
    s->have_source = (bsdr_faceswap_set_source_rgb(s->fs, rgb, w, h) == 0);
    return s->have_source ? 0 : -1;
}

/* The host video effect (order 10): swap faces in the NV12 encoder frame IN PLACE, same dims. */
static void fs_apply(void *user, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride, int width, int height) {
    fs_state *s = (fs_state *)user;
    if (width <= 0 || height <= 0) return;
    if ((s->tick++ % 30) == 0) fs_refresh(s);            /* ~0.5 s cadence; off the ONNX cost */
    if (!s->on || !s->fs || !s->have_source || !bsdr_faceswap_ready(s->fs)) return;

    if (!s->rgb || s->rgb_w != width || s->rgb_h != height) {
        free(s->rgb);
        s->rgb = (uint8_t *)malloc((size_t)width * height * 3);
        s->rgb_w = width; s->rgb_h = height;
        if (!s->rgb) { s->rgb_w = s->rgb_h = 0; return; }
    }
    nv12_to_rgb(y, y_stride, uv, uv_stride, width, height, s->rgb);
    /* Only convert back when a face was actually swapped — leaves no-face frames untouched (no tint). */
    if (bsdr_faceswap_process_rgb(s->fs, s->rgb, width, height) > 0)
        rgb_to_nv12(s->rgb, width, height, y, y_stride, uv, uv_stride);
}

static int fs_init(const bsdr_plugin_host *host, void **state) {
    fs_state *s = (fs_state *)calloc(1, sizeof *s);
    if (!s) return 1;
    s->host = host;
    g_host = host;                     /* for the log shim */
    s->tier = -1;                      /* force a load on first enable */
    if (host->video_fx_register) host->video_fx_register(fs_apply, s, 10);   /* order 10 = before 2D->3D */
    if (host->face_fx_register) {   /* RGB interface for a host with RGB frames (Android GL) */
        static const bsdr_face_fx FF = { .process = fs_process_rgb, .set_source = fs_set_source_rgb };
        bsdr_face_fx fx = FF; fx.user = s;
        host->face_fx_register(&fx);
    }
    host->log(1, "bsdr.plugin.faceswap", "loaded — owns the face-swap effect (SCRFD + ArcFace + inswapper)");
    *state = s;
    return 0;
}

static void fs_shutdown(void *state) {
    fs_state *s = (fs_state *)state;
    /* the host also clears the video/face effects on unload, but be explicit + free the engine/buffers */
    if (s && s->host && s->host->video_fx_register) s->host->video_fx_register(NULL, s, 10);
    if (s && s->host && s->host->face_fx_register)  s->host->face_fx_register(NULL);
    if (s && s->fs) bsdr_faceswap_close(s->fs);
    if (s) free(s->rgb);
    free(s);
    g_host = NULL;
}

/* The Face-swap control card, injected once as a persistent script (.ui_script). Builds the card +
 * wires it to the CORE endpoints /api/faceswap + /api/faceswap-download/import and the shared file
 * browser (window.fbBrowse); the faceswap CONFIG + model store stay in the core. */
static const char FS_UI_JS[] =
"(function(){if(document.getElementById('fscard'))return;\n"
"var api=window.bsdrUI.api;\n"
"var c=document.createElement('div');c.className='card';c.id='fscard';\n"
"c.innerHTML='<h2>Face swap <span class=badge>GPU</span></h2>'\n"
"+'<div class=row><label style=width:auto><input id=fson type=checkbox style=width:auto> Swap faces in the stream to a source image (realtime deepfake)</label></div>'\n"
"+'<div class=row><label style=width:120px;color:var(--muted)>Source image</label><input id=fssrc class=grow><button id=fsbrowse style=width:auto>Browse&#8230;</button></div>'\n"
"+'<div class=row><label style=width:120px;color:var(--muted)>Compute</label><select id=fstier class=grow><option value=1>CPU</option><option value=2>GPU (CUDA/DirectML/CoreML/NNAPI)</option><option value=3>GPU (high)</option></select></div>'\n"
"+'<div id=fsstat class=hint>face swap: off</div>'\n"
"+'<div class=hint>Needs the insightface models (det_10g.onnx, w600k_r50.onnx, inswapper_128.onnx) in the faceswap model dir; they are non-commercial so not bundled &#8212; Download fetches them, or import a zip. Runs on the CPU encode path; use a GPU tier for realtime.</div>'\n"
"+'<div class=row><button id=fsdl style=width:auto>Download models</button><span id=fsmodels class=hint style=margin-left:8px>models&#8230;</span></div>'\n"
"+'<div class=row><input id=fszip class=grow><button id=fsimp style=width:auto>Import model zip</button></div>';\n"
"(document.getElementById('pluginpanels')||document.body).appendChild(c);\n"
"var $=function(id){return document.getElementById(id);};\n"
"$('fssrc').placeholder='/path/to/face.jpg on this machine';\n"
"$('fszip').placeholder='path to a models .zip (buffalo_l.zip) on this machine';\n"
"function post(){api('/api/faceswap',{on:$('fson').checked?1:0,tier:+$('fstier').value,source:$('fssrc').value});}\n"
"$('fson').onchange=post;$('fssrc').onchange=post;$('fstier').onchange=post;\n"
"$('fsbrowse').onclick=function(){window.fbTarget='fssrc';window.fbKind='image';var p=$('fssrc').value||'';var d=p.lastIndexOf('/')>0?p.substring(0,p.lastIndexOf('/')):'';window.fbBrowse(d);};\n"
"$('fsdl').onclick=function(){api('/api/faceswap-download',{}).then(function(r){if(!r||r.ok!==true)alert('cannot start download');});};\n"
"$('fsimp').onclick=function(){if(!$('fszip').value)return;api('/api/faceswap-import',{path:$('fszip').value}).then(function(r){alert(r&&r.imported>=0?('imported '+r.imported+' model(s)'):'import failed');});};\n"
"function renderFsModels(m){var e=$('fsmodels');if(!m){e.textContent='';return;}var d=m.dl;\n"
"var rows=(m.files||[]).map(function(f){return f.name+': '+(f.present?'✓':'✗');}).join('  ');\n"
"if(d&&d.active)rows+='  ↓ '+(d.name||'')+' '+(d.pct>=0?d.pct+'%':((d.done/1048576).toFixed(0)+' MB'));\n"
"else if(d&&d.err)rows+='  <span style=color:#f85149>'+d.err+'</span>';\n"
"else if(m.ready)rows='<span style=color:#3fb950>✓ all models present</span>';\n"
"e.innerHTML=rows+'  <span style=color:var(--muted)>('+(m.dir||'?')+')</span>';}\n"
"window.bsdrUI.onStatus(function(s){if(s.faceswap){var fx=s.faceswap;\n"
"if(document.activeElement!==$('fson'))$('fson').checked=fx.on;\n"
"if(document.activeElement!==$('fssrc'))$('fssrc').value=fx.source||'';\n"
"if(document.activeElement!==$('fstier')&&fx.tier)$('fstier').value=fx.tier;\n"
"$('fsstat').textContent='face swap: '+(fx.status||(fx.on?'on':'off'));\n""window.bsdrUI.badge('fscard',!!fx.on,fx.on?'on':'off');\n""renderFsModels(fx.models);}});\n"
"})();\n"
;

static int fs_http(void *state, const char *method, const char *path, const char *body, void *conn) {
    fs_state *s = (fs_state *)state;
    (void)method; (void)body;
    if (strstr(path, "/ui.js")) {   /* serve the persistent control-card script */
        if (s->host->http_respond) s->host->http_respond(conn, 200, "application/javascript", FS_UI_JS, sizeof FS_UI_JS - 1);
        return 1;
    }
    return 0;
}

static const bsdr_plugin FACESWAP = {
    .abi          = BSDR_PLUGIN_ABI,
    .abi_max      = 0,
    .struct_size  = sizeof(bsdr_plugin),
    .name         = "faceswap",
    .version      = "0.1.0",
    .description  = "Realtime face swap / deepfake onto the streamed video (SCRFD + ArcFace + inswapper, ONNX)",
    .init         = fs_init,
    .shutdown     = fs_shutdown,
    .http         = fs_http,
    .ui_script    = "/api/plugin/faceswap/ui.js",
};

const bsdr_plugin *bsdr_plugin_register(void) { return &FACESWAP; }
