/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* voicestore.c — AI voice-changer model store: engine base models + the user voice library.
 * See voicestore.h. Reuses bsdr_http_download + miniz; own single-slot async download channel. */
#include "bsdr/voicestore.h"
#include "bsdr/httpc.h"
#include "bsdr/json.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#  define MKDIR(p) _mkdir(p)
#  define PATHSEP '\\'
#else
#  include <unistd.h>
#  include <dirent.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define PATHSEP '/'
#endif

#include "miniz.h"

/* ---- engine base-model catalog ---------------------------------------------------------------
 * URLs are best-effort community mirrors (unpinned: sha ""), so the reliable paths are the web-UI
 * "download from URL" and "import zip". Both models are permissive to REDISTRIBUTE as weights the
 * user fetches; bsdrX ships none. ContentVec (vec-768-layer-12) = the RVC content encoder; RMVPE =
 * the pitch extractor used by the GPU tiers (the CPU tier has a built-in DSP pitch tracker). */
static const bsdr_vbase_info VBASE[BSDR_VBASE_COUNT] = {
    { "contentvec", "contentvec.onnx",
      "https://huggingface.co/NaruseMioShirakana/MoeSS-SUBModel/resolve/main/vec-768-layer-12.onnx",
      "", 190 },
    { "rmvpe", "rmvpe.onnx",
      "https://huggingface.co/lj1995/VoiceConversionWebUI/resolve/main/rmvpe.onnx",
      "", 345 },
};

const bsdr_vbase_info *bsdr_voice_base_info(bsdr_vbase which) {
    if (which < 0 || which >= BSDR_VBASE_COUNT) return NULL;
    return &VBASE[which];
}

/* ---- small fs helpers ------------------------------------------------------------------------- */
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFREG); }
static long file_size(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

int bsdr_voice_dir(char *out, size_t cap) {
    char base[768];
    bsdr_model_dir(base, sizeof base);                 /* the shared model cache root */
    snprintf(out, cap, "%s%cvoice", base, PATHSEP);
    MKDIR(out);
    char vv[1024]; snprintf(vv, sizeof vv, "%s%cvoices", out, PATHSEP); MKDIR(vv);   /* library subdir */
    return 0;
}
static int voices_dir(char *out, size_t cap) {
    char d[768]; bsdr_voice_dir(d, sizeof d);
    snprintf(out, cap, "%s%cvoices", d, PATHSEP);
    return 0;
}

int bsdr_voice_base_path(bsdr_vbase which, char *out, size_t cap) {
    const bsdr_vbase_info *b = bsdr_voice_base_info(which); if (!b) return -1;
    char d[768]; bsdr_voice_dir(d, sizeof d);
    snprintf(out, cap, "%s%c%s", d, PATHSEP, b->filename);
    return 0;
}
int bsdr_voice_base_present(bsdr_vbase which) {
    char p[1024]; if (bsdr_voice_base_path(which, p, sizeof p) != 0) return 0;
    return file_exists(p);
}
int bsdr_voice_base_ready(void) { return bsdr_voice_base_present(BSDR_VBASE_CONTENT); }

/* ---- single-slot async download channel (shared by base models + voice fetches) --------------- */
static bsdr_mutex   *g_vlock;
static bsdr_model_dl g_vdl;
static char g_vurl[1024], g_vdest[1088], g_vname_job[96], g_vsidecar[1200];
static int  g_vjob;   /* 0 = base models (multi), 1 = single voice */

static void vlock_init(void) { if (!g_vlock) g_vlock = bsdr_mutex_new(); }
static void v_progress(size_t done, size_t total) {
    if (!g_vlock) return;
    bsdr_mutex_lock(g_vlock);
    g_vdl.done = (long)done; g_vdl.total = (long)total;
    g_vdl.pct = total ? (int)((done * 100) / total) : -1;
    bsdr_mutex_unlock(g_vlock);
}
static void v_set_name(const char *nm) {
    bsdr_mutex_lock(g_vlock); snprintf(g_vdl.name, sizeof g_vdl.name, "%s", nm); bsdr_mutex_unlock(g_vlock);
}
static void v_finish(int ok, const char *err) {
    bsdr_mutex_lock(g_vlock);
    g_vdl.active = 0; g_vdl.ok = ok;
    if (ok) { g_vdl.pct = 100; g_vdl.err[0] = 0; } else snprintf(g_vdl.err, sizeof g_vdl.err, "%s", err ? err : "error");
    bsdr_mutex_unlock(g_vlock);
}

static int download_to(const char *url, const char *dest) {   /* atomic .part -> dest */
    char tmp[1152]; snprintf(tmp, sizeof tmp, "%s.part", dest);
    if (bsdr_http_download(url, tmp, v_progress) != 0 || !file_exists(tmp)) { remove(tmp); return -1; }
    remove(dest);
    if (rename(tmp, dest) != 0) { remove(tmp); return -1; }
    return 0;
}

static void base_worker(void *arg) {
    (void)arg;
    int ok = 1; char err[96] = "";
    for (int i = 0; i < BSDR_VBASE_COUNT && ok; i++) {
        if (bsdr_voice_base_present((bsdr_vbase)i)) continue;
        const bsdr_vbase_info *b = &VBASE[i];
        if (!b->url[0]) continue;                         /* supply/import-only model: skip */
        v_set_name(b->name);
        char dest[1024]; bsdr_voice_base_path((bsdr_vbase)i, dest, sizeof dest);
        if (download_to(b->url, dest) != 0) { ok = 0; snprintf(err, sizeof err, "%s download failed", b->name); }
    }
    v_finish(ok, err);
    if (ok) BSDR_INFO("bsdr.voice", "engine base models ready");
    else    BSDR_WARN("bsdr.voice", "base model download failed (%s)", err);
}

static void voice_worker(void *arg) {
    (void)arg;
    int ok = (download_to(g_vurl, g_vdest) == 0);
    if (ok && g_vsidecar[0]) {                            /* write the display-name sidecar */
        FILE *f = fopen(g_vsidecar, "w");
        if (f) { fprintf(f, "{\"name\":\"%s\",\"needs_f0\":1}\n", g_vname_job); fclose(f); }
    }
    v_finish(ok, ok ? NULL : "voice download failed");
    if (ok) BSDR_INFO("bsdr.voice", "voice '%s' downloaded", g_vname_job);
    else    { remove(g_vdest); BSDR_WARN("bsdr.voice", "voice download failed: %s", g_vurl); }
}

static int start_job(int kind, void (*fn)(void *)) {
    vlock_init(); if (!g_vlock) return -1;
    bsdr_mutex_lock(g_vlock);
    if (g_vdl.active) { bsdr_mutex_unlock(g_vlock); return 0; }   /* one at a time */
    memset(&g_vdl, 0, sizeof g_vdl);
    g_vdl.active = 1; g_vdl.pct = -1; g_vjob = kind;
    bsdr_mutex_unlock(g_vlock);
    if (!bsdr_thread_start_detached(fn, NULL)) { v_finish(0, "cannot start download thread"); return -1; }
    return 0;
}

int bsdr_voice_base_download_start(void) {
    if (bsdr_voice_base_ready() && bsdr_voice_base_present(BSDR_VBASE_RMVPE)) return 0;
    return start_job(0, base_worker);
}
void bsdr_voice_download_state(bsdr_model_dl *out) {
    if (!out) return;
    vlock_init(); if (!g_vlock) { memset(out, 0, sizeof *out); return; }
    bsdr_mutex_lock(g_vlock); *out = g_vdl; bsdr_mutex_unlock(g_vlock);
}

/* ---- voice library --------------------------------------------------------------------------- */
/* Sanitize an id to a safe basename (alnum + - _), max 63 chars. */
static void sanitize_id(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < cap && j < 63; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out[j++] = c;
        else if (c == ' ') out[j++] = '_';
    }
    if (!j) out[j++] = 'v';
    out[j] = 0;
}

int bsdr_voice_path(const char *id, char *out, size_t cap) {
    if (!id || !id[0]) return -1;
    char sid[64]; sanitize_id(id, sid, sizeof sid);
    char vd[1024]; voices_dir(vd, sizeof vd);
    snprintf(out, cap, "%s%c%s.onnx", vd, PATHSEP, sid);
    return file_exists(out) ? 0 : -1;
}

int bsdr_voice_get(const char *id, bsdr_voice_entry *out) {
    if (!id || !out) return -1;
    char sid[64]; sanitize_id(id, sid, sizeof sid);
    char path[1200];
    if (bsdr_voice_path(sid, path, sizeof path) != 0) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", sid);
    snprintf(out->name, sizeof out->name, "%s", sid);
    out->sample_rate = 40000; out->needs_f0 = 1; out->version = 0;
    out->size_bytes = file_size(path);
    /* sidecar <id>.json (optional): name / sample_rate / needs_f0 / version */
    char vd[1024]; voices_dir(vd, sizeof vd);
    char scar[1200]; snprintf(scar, sizeof scar, "%s%c%s.json", vd, PATHSEP, sid);
    FILE *f = fopen(scar, "rb");
    if (f) {
        char buf[2048]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        char nm[96]; if (bsdr_json_get_str(buf, "name", nm, sizeof nm) && nm[0]) snprintf(out->name, sizeof out->name, "%s", nm);
        double d;
        if (bsdr_json_get_double(buf, "sample_rate", &d) && d > 0) out->sample_rate = (int)d;
        if (bsdr_json_get_double(buf, "needs_f0", &d)) out->needs_f0 = d != 0;
        if (bsdr_json_get_double(buf, "version", &d)) out->version = (int)d;
    }
    return 0;
}

int bsdr_voice_list(bsdr_voice_entry *out, int cap) {
    char vd[1024]; voices_dir(vd, sizeof vd);
    int n = 0;
#ifdef _WIN32
    char glob[1200]; snprintf(glob, sizeof glob, "%s\\*.onnx", vd);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (n >= cap) break;
            const char *ext = strrchr(fd.cFileName, '.');
            int idlen = ext ? (int)(ext - fd.cFileName) : (int)strlen(fd.cFileName);
            char id[64]; snprintf(id, sizeof id, "%.*s", idlen, fd.cFileName);
            if (bsdr_voice_get(id, &out[n]) == 0) n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *dir = opendir(vd);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) && n < cap) {
            const char *ext = strrchr(de->d_name, '.');
            if (!ext || strcmp(ext, ".onnx") != 0) continue;
            char id[64]; snprintf(id, sizeof id, "%.*s", (int)(ext - de->d_name), de->d_name);
            if (bsdr_voice_get(id, &out[n]) == 0) n++;
        }
        closedir(dir);
    }
#endif
    return n;
}

int bsdr_voice_add_file(const char *src_path, const char *id, const char *name,
                        int sample_rate, int needs_f0) {
    if (!src_path || !id) return -1;
    if (!file_exists(src_path)) { BSDR_WARN("bsdr.voice", "add: source %s missing", src_path); return -1; }
    char sid[64]; sanitize_id(id, sid, sizeof sid);
    char vd[1024]; voices_dir(vd, sizeof vd);
    char dest[1200]; snprintf(dest, sizeof dest, "%s%c%s.onnx", vd, PATHSEP, sid);
    /* copy src -> dest */
    FILE *in = fopen(src_path, "rb"); if (!in) return -1;
    char tmp[1264]; snprintf(tmp, sizeof tmp, "%s.part", dest);
    FILE *outf = fopen(tmp, "wb"); if (!outf) { fclose(in); return -1; }
    char buf[65536]; size_t r; int ok = 1;
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, r, outf) != r) { ok = 0; break; }
    fclose(in); fclose(outf);
    if (!ok) { remove(tmp); return -1; }
    remove(dest);
    if (rename(tmp, dest) != 0) { remove(tmp); return -1; }
    /* sidecar */
    char scar[1200]; snprintf(scar, sizeof scar, "%s%c%s.json", vd, PATHSEP, sid);
    FILE *sf = fopen(scar, "w");
    if (sf) {
        fprintf(sf, "{\"name\":\"%s\",\"sample_rate\":%d,\"needs_f0\":%d}\n",
                (name && name[0]) ? name : sid, sample_rate > 0 ? sample_rate : 40000, needs_f0 ? 1 : 0);
        fclose(sf);
    }
    BSDR_INFO("bsdr.voice", "added voice '%s' (%s)", sid, dest);
    return 0;
}

int bsdr_voice_delete(const char *id) {
    if (!id) return -1;
    char sid[64]; sanitize_id(id, sid, sizeof sid);
    char vd[1024]; voices_dir(vd, sizeof vd);
    char p[1200];
    snprintf(p, sizeof p, "%s%c%s.onnx",  vd, PATHSEP, sid); remove(p);
    snprintf(p, sizeof p, "%s%c%s.json",  vd, PATHSEP, sid); remove(p);
    snprintf(p, sizeof p, "%s%c%s.index", vd, PATHSEP, sid); remove(p);
    BSDR_INFO("bsdr.voice", "deleted voice '%s'", sid);
    return 0;
}

int bsdr_voice_download_start(const char *url, const char *id, const char *name) {
    if (!url || !url[0] || !id || !id[0]) return -1;
    char sid[64]; sanitize_id(id, sid, sizeof sid);
    char vd[900]; voices_dir(vd, sizeof vd);   /* <cache>/voice/voices — well under 900 in practice */
    vlock_init(); if (!g_vlock) return -1;
    bsdr_mutex_lock(g_vlock);
    if (g_vdl.active) { bsdr_mutex_unlock(g_vlock); return 0; }
    snprintf(g_vurl,     sizeof g_vurl,     "%s", url);
    snprintf(g_vdest,    sizeof g_vdest,    "%s%c%s.onnx", vd, PATHSEP, sid);
    snprintf(g_vsidecar, sizeof g_vsidecar, "%s%c%s.json", vd, PATHSEP, sid);
    snprintf(g_vname_job,sizeof g_vname_job,"%s", (name && name[0]) ? name : sid);
    bsdr_mutex_unlock(g_vlock);
    return start_job(1, voice_worker);
}

/* ---- base-model zip import ------------------------------------------------------------------- */
int bsdr_voice_base_import_zip(const char *zip_path) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip_path, 0)) { BSDR_ERROR("bsdr.voice", "cannot open zip %s", zip_path); return -1; }
    int imported = 0;
    for (int i = 0; i < BSDR_VBASE_COUNT; i++) {
        const bsdr_vbase_info *b = &VBASE[i];
        int idx = mz_zip_reader_locate_file(&z, b->filename, NULL, 0);
        if (idx < 0) continue;
        char dest[1024]; bsdr_voice_base_path((bsdr_vbase)i, dest, sizeof dest);
        char tmp[1088]; snprintf(tmp, sizeof tmp, "%s.part", dest);
        if (mz_zip_reader_extract_to_file(&z, idx, tmp, 0)) {
            remove(dest); if (rename(tmp, dest) == 0) { imported++; BSDR_INFO("bsdr.voice", "imported %s", b->name); }
            else remove(tmp);
        }
    }
    mz_zip_reader_end(&z);
    return imported ? imported : -1;
}

/* ---- built-in online catalog ----------------------------------------------------------------- */
/* A small curated set of downloadable example voices (user-fetched weights). Arbitrary URLs also
 * work via bsdr_voice_download_start(). URLs are unpinned mirrors — may need updating. */
static const bsdr_voice_catalog_entry VCATALOG[] = {
    { "example-female", "Example — Female", "", 40000, 55 },
    { "example-male",   "Example — Male",   "", 40000, 55 },
};
const bsdr_voice_catalog_entry *bsdr_voice_catalog(int *count) {
    if (count) *count = (int)(sizeof VCATALOG / sizeof VCATALOG[0]);
    return VCATALOG;
}
