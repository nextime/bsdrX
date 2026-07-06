/* model_store.c — depth-model catalog, cache, on-demand download, and manual zip import.
 * See model_store.h. Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
 */
#include "bsdr/model_store.h"
#include "bsdr/httpc.h"
#include "bsdr/log.h"
#include "bsdr/platform.h"   /* bsdr_thread_start_detached + bsdr_mutex (async download) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <openssl/evp.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define PATHSEP '\\'
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define PATHSEP '/'
#endif

#include "miniz.h"

/* ---- catalog: single source of truth (filename / url / sha / input size / preprocess) ---------
 * All three are GPLv3-redistributable (Apache/MIT). Depth-Anything-V2 Base/Large are CC-BY-NC and
 * must NOT appear here. sha256 "" = not pinned yet (verification skipped until pinned in packaging).
 * Tier 2/3 URLs are pinned during model-zip packaging (Phase D); until then those tiers require a
 * manual zip import. */
static const bsdr_model_info CATALOG[3] = {
    /* tier 1 — Depth-Anything-V2-Small (Apache-2.0): 518px (/14), ImageNet mean/std. */
    { 1, "depth-anything-v2-small", "depth-anything-v2-small.onnx",
      "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx",
      "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c",
      518, 99, { 0.485f, 0.456f, 0.406f }, { 0.229f, 0.224f, 0.225f } },
    /* tier 2 — MiDaS DPT-Hybrid (MIT, Intel via Xenova): 384px, DPTImageProcessor mean/std 0.5. */
    { 2, "midas-dpt-hybrid", "midas-dpt-hybrid-384.onnx",
      "https://huggingface.co/Xenova/dpt-hybrid-midas/resolve/main/onnx/model.onnx",
      "2eca68239006c64af94bdfa68464f34d7627c3dca7bb02932636e616c55a39ff",
      384, 490, { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f } },
    /* tier 3 — MiDaS DPT-Large (MIT, Intel via Xenova): 384px, DPTImageProcessor mean/std 0.5. */
    { 3, "midas-dpt-large", "midas-dpt-large-384.onnx",
      "https://huggingface.co/Xenova/dpt-large/resolve/main/onnx/model.onnx",
      "68890009c34e0c888054ad7ec323a415288ff4392eca939206037210667cfb33",
      384, 1300, { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f } },
};

const bsdr_model_info *bsdr_model_for_tier(int tier) {
    if (tier < 1 || tier > 3) return NULL;
    return &CATALOG[tier - 1];
}

/* ---- cache dir + fs helpers ------------------------------------------------------------------ */
int bsdr_model_dir(char *out, size_t cap) {
    const char *ov = getenv("BSDR_MODEL_DIR");     /* --threed-model-dir sets this */
    if (ov && *ov) { snprintf(out, cap, "%s", ov); }
#if defined(_WIN32)
    else { const char *b = getenv("LOCALAPPDATA"); if (!b || !*b) b = getenv("TEMP");
           snprintf(out, cap, "%s\\bsdrX\\models", b ? b : "."); }
#elif defined(__APPLE__)
    else { const char *h = getenv("HOME"); snprintf(out, cap, "%s/Library/Caches/bsdrX/models", h ? h : "."); }
#elif defined(__ANDROID__)
    else { snprintf(out, cap, "/data/local/tmp/bsdrX/models"); }   /* app overrides via BSDR_MODEL_DIR */
#else
    else { const char *x = getenv("XDG_CACHE_HOME");
           if (x && *x) snprintf(out, cap, "%s/bsdrX/models", x);
           else { const char *h = getenv("HOME"); snprintf(out, cap, "%s/.cache/bsdrX/models", h ? h : "."); } }
#endif
    /* mkdir -p */
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", out);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { char sv = *p; *p = 0; MKDIR(tmp); *p = sv; }
    }
    MKDIR(tmp);
    return 0;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFREG); }

static int sha256_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    EVP_MD_CTX *c = EVP_MD_CTX_new(); if (!c) { fclose(f); return -1; }
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    unsigned char buf[65536]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) EVP_DigestUpdate(c, buf, r);
    unsigned char md[32]; unsigned int n = 0; EVP_DigestFinal_ex(c, md, &n);
    EVP_MD_CTX_free(c); fclose(f);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
    hex[64] = 0; return 0;
}
/* verify a file against a pinned sha (empty sha -> accept). returns 1 if ok. */
static int sha_ok(const char *path, const char *want) {
    if (!want || !*want) return 1;
    char have[65]; if (sha256_file(path, have) != 0) return 0;
    return strcmp(have, want) == 0;
}

int bsdr_model_present(int tier) {
    const bsdr_model_info *mi = bsdr_model_for_tier(tier); if (!mi) return 0;
    char dir[768], path[1024]; bsdr_model_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s%c%s", dir, PATHSEP, mi->filename);
    return file_exists(path) && sha_ok(path, mi->sha256);
}

int bsdr_model_resolve(int tier, int allow_download, char *path, size_t cap) {
    const bsdr_model_info *mi = bsdr_model_for_tier(tier); if (!mi) return -1;
    char dir[768]; bsdr_model_dir(dir, sizeof dir);
    snprintf(path, cap, "%s%c%s", dir, PATHSEP, mi->filename);
    if (file_exists(path) && sha_ok(path, mi->sha256)) return 0;

    if (allow_download && mi->url[0]) {
        char tmp[1088]; snprintf(tmp, sizeof tmp, "%s.part", path);
        BSDR_INFO("bsdr.model", "downloading %s …", mi->name);
        if (bsdr_http_download(mi->url, tmp, NULL) == 0 && sha_ok(tmp, mi->sha256)) {
            remove(path); if (rename(tmp, path) == 0) return 0;
        }
        remove(tmp);
        BSDR_WARN("bsdr.model", "download of %s failed", mi->name);
    }
    return -1;
}

/* ---- manual zip import (miniz) --------------------------------------------------------------- */
/* Accepts a per-model zip or the all-three zip: for each catalog model whose canonical filename is
 * present in the zip, extract it into the cache (verified against the pinned sha, if any). */
int bsdr_model_import_zip(const char *zip_path) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip_path, 0)) {
        BSDR_ERROR("bsdr.model", "cannot open model zip %s", zip_path); return -1;
    }
    char dir[768]; bsdr_model_dir(dir, sizeof dir);
    int imported = 0;
    for (int tier = 1; tier <= 3; tier++) {
        const bsdr_model_info *mi = bsdr_model_for_tier(tier);
        int idx = mz_zip_reader_locate_file(&z, mi->filename, NULL, 0);
        if (idx < 0) continue;                       /* this model not in this zip */
        char dest[1024], tmp[1088];
        snprintf(dest, sizeof dest, "%s%c%s", dir, PATHSEP, mi->filename);
        snprintf(tmp,  sizeof tmp,  "%s.part", dest);
        if (!mz_zip_reader_extract_to_file(&z, idx, tmp, 0)) {
            BSDR_WARN("bsdr.model", "extract %s failed", mi->filename); continue;
        }
        if (!sha_ok(tmp, mi->sha256)) {
            BSDR_WARN("bsdr.model", "%s checksum mismatch — rejected", mi->filename); remove(tmp); continue;
        }
        remove(dest);
        if (rename(tmp, dest) == 0) { imported++; BSDR_INFO("bsdr.model", "imported %s -> %s", mi->name, dest); }
        else remove(tmp);
    }
    mz_zip_reader_end(&z);
    if (!imported) BSDR_WARN("bsdr.model", "no known models found in %s", zip_path);
    return imported ? imported : -1;
}

/* ---- async background download (web-UI driven, progress-reported) ----------------------------- */
/* One download at a time. The state is a file-scope singleton because bsdr_http_download's progress
 * callback carries no user pointer; the guard below ensures only one worker touches it at once. */
static bsdr_mutex   *g_dl_lock;      /* created lazily from the (single) requesting thread */
static bsdr_model_dl g_dl;           /* guarded by g_dl_lock once created */

static void dl_lock_init(void) { if (!g_dl_lock) g_dl_lock = bsdr_mutex_new(); }

static void dl_progress(size_t done, size_t total) {
    if (!g_dl_lock) return;
    bsdr_mutex_lock(g_dl_lock);
    g_dl.done  = (long)done;
    g_dl.total = (long)total;
    g_dl.pct   = total ? (int)((done * 100) / total) : -1;
    bsdr_mutex_unlock(g_dl_lock);
}

static void dl_worker(void *arg) {
    int tier = (int)(intptr_t)arg;
    const bsdr_model_info *mi = bsdr_model_for_tier(tier);
    char dir[768], path[1024], tmp[1088];
    bsdr_model_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s%c%s", dir, PATHSEP, mi->filename);
    snprintf(tmp,  sizeof tmp,  "%s.part", path);
    BSDR_INFO("bsdr.model", "downloading %s (~%d MB) …", mi->name, mi->approx_mb);
    int rc = bsdr_http_download(mi->url, tmp, dl_progress);
    int ok = (rc == 0 && sha_ok(tmp, mi->sha256));
    if (ok) { remove(path); if (rename(tmp, path) != 0) ok = 0; }
    if (!ok) remove(tmp);
    bsdr_mutex_lock(g_dl_lock);
    g_dl.active = 0;
    g_dl.ok = ok;
    if (ok) { g_dl.pct = 100; g_dl.err[0] = 0; }
    else snprintf(g_dl.err, sizeof g_dl.err, "%s", rc != 0 ? "network/download error" : "checksum mismatch");
    bsdr_mutex_unlock(g_dl_lock);
    if (ok) BSDR_INFO("bsdr.model", "%s ready", mi->name);
    else    BSDR_WARN("bsdr.model", "download of %s failed (%s)", mi->name, g_dl.err);
}

int bsdr_model_download_start(int tier) {
    const bsdr_model_info *mi = bsdr_model_for_tier(tier);
    if (!mi) return -1;
    dl_lock_init();
    if (!g_dl_lock) return -1;
    bsdr_mutex_lock(g_dl_lock);
    if (g_dl.active)            { bsdr_mutex_unlock(g_dl_lock); return 0; }  /* one at a time */
    if (bsdr_model_present(tier)){ bsdr_mutex_unlock(g_dl_lock); return 0; } /* already cached */
    if (!mi->url[0])            { bsdr_mutex_unlock(g_dl_lock); return -1; } /* no URL -> import zip */
    memset(&g_dl, 0, sizeof g_dl);
    g_dl.active = 1; g_dl.tier = tier; g_dl.pct = -1;
    snprintf(g_dl.name, sizeof g_dl.name, "%s", mi->name);
    bsdr_mutex_unlock(g_dl_lock);
    if (!bsdr_thread_start_detached(dl_worker, (void *)(intptr_t)tier)) {
        bsdr_mutex_lock(g_dl_lock); g_dl.active = 0;
        snprintf(g_dl.err, sizeof g_dl.err, "cannot start download thread"); bsdr_mutex_unlock(g_dl_lock);
        return -1;
    }
    return 0;
}

void bsdr_model_download_state(bsdr_model_dl *out) {
    if (!out) return;
    dl_lock_init();
    if (!g_dl_lock) { memset(out, 0, sizeof *out); return; }
    bsdr_mutex_lock(g_dl_lock);
    *out = g_dl;
    bsdr_mutex_unlock(g_dl_lock);
}

/* ---- face-swap models (buffalo_l det/rec + inswapper) ---------------------------------------- */
const char *const bsdr_faceswap_files[BSDR_FACESWAP_NFILES] = {
    "det_10g.onnx", "w600k_r50.onnx", "inswapper_128.onnx"
};
/* det_10g + w600k_r50 ship inside the official insightface buffalo_l release zip; inswapper is a
 * standalone file on a HuggingFace mirror. Both non-commercial — mechanism only, no sha pin. */
#define FS_BUFFALO_URL   "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_l.zip"
#define FS_INSWAPPER_URL "https://huggingface.co/ezioruan/inswapper_128.onnx/resolve/main/inswapper_128.onnx"

int bsdr_faceswap_model_dir(char *out, size_t cap) {
    char base[768]; bsdr_model_dir(base, sizeof base);
    snprintf(out, cap, "%s%cfaceswap", base, PATHSEP);
    MKDIR(out);
    return 0;
}
int bsdr_faceswap_file_present(const char *filename) {
    char dir[900], path[1200]; bsdr_faceswap_model_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s%c%s", dir, PATHSEP, filename);
    return file_exists(path);
}
int bsdr_faceswap_models_ready(void) {
    for (int i = 0; i < BSDR_FACESWAP_NFILES; i++)
        if (!bsdr_faceswap_file_present(bsdr_faceswap_files[i])) return 0;
    return 1;
}

static bsdr_mutex   *g_fs_lock;
static bsdr_model_dl g_fs_dl;
static void fs_lock_init(void) { if (!g_fs_lock) g_fs_lock = bsdr_mutex_new(); }
static void fs_progress(size_t done, size_t total) {
    if (!g_fs_lock) return;
    bsdr_mutex_lock(g_fs_lock);
    g_fs_dl.done = (long)done; g_fs_dl.total = (long)total;
    g_fs_dl.pct = total ? (int)((done * 100) / total) : -1;
    bsdr_mutex_unlock(g_fs_lock);
}
static void fs_set_name(const char *nm) {
    bsdr_mutex_lock(g_fs_lock); snprintf(g_fs_dl.name, sizeof g_fs_dl.name, "%s", nm); bsdr_mutex_unlock(g_fs_lock);
}
/* Extract det_10g.onnx + w600k_r50.onnx out of a buffalo_l zip into the face-swap dir. */
static int fs_extract_buffalo(const char *zip_path, const char *dir) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip_path, 0)) return -1;
    int got = 0;
    for (int i = 0; i < 2; i++) {   /* det_10g + w600k_r50 (buffalo_l stores them at the zip root) */
        const char *want = bsdr_faceswap_files[i];
        int idx = mz_zip_reader_locate_file(&z, want, NULL, 0);
        if (idx < 0) { char alt[64]; snprintf(alt, sizeof alt, "buffalo_l/%s", want);
                       idx = mz_zip_reader_locate_file(&z, alt, NULL, 0); }
        if (idx < 0) continue;
        char dest[1200]; snprintf(dest, sizeof dest, "%s%c%s", dir, PATHSEP, want);
        if (mz_zip_reader_extract_to_file(&z, idx, dest, 0)) got++;
    }
    mz_zip_reader_end(&z);
    return got == 2 ? 0 : -1;
}
static void fs_worker(void *arg) {
    (void)arg;
    char dir[900]; bsdr_faceswap_model_dir(dir, sizeof dir);
    int ok = 1; char err[96] = "";
    /* 1) buffalo_l (det_10g + w600k_r50) if either is missing */
    if (!bsdr_faceswap_file_present("det_10g.onnx") || !bsdr_faceswap_file_present("w600k_r50.onnx")) {
        fs_set_name("buffalo_l (det + rec)");
        char zip[1200]; snprintf(zip, sizeof zip, "%s%cbuffalo_l.zip.part", dir, PATHSEP);
        if (bsdr_http_download(FS_BUFFALO_URL, zip, fs_progress) != 0) { ok = 0; snprintf(err, sizeof err, "buffalo_l download failed"); }
        else if (fs_extract_buffalo(zip, dir) != 0) { ok = 0; snprintf(err, sizeof err, "buffalo_l extract failed"); }
        remove(zip);
    }
    /* 2) inswapper_128.onnx if missing */
    if (ok && !bsdr_faceswap_file_present("inswapper_128.onnx")) {
        fs_set_name("inswapper_128");
        char dest[1200], tmp[1264];
        snprintf(dest, sizeof dest, "%s%cinswapper_128.onnx", dir, PATHSEP);
        snprintf(tmp,  sizeof tmp,  "%s.part", dest);
        if (bsdr_http_download(FS_INSWAPPER_URL, tmp, fs_progress) == 0 && file_exists(tmp)) {
            remove(dest); if (rename(tmp, dest) != 0) { ok = 0; snprintf(err, sizeof err, "inswapper save failed"); }
        } else { ok = 0; remove(tmp); snprintf(err, sizeof err, "inswapper download failed"); }
    }
    bsdr_mutex_lock(g_fs_lock);
    g_fs_dl.active = 0; g_fs_dl.ok = ok;
    if (ok) { g_fs_dl.pct = 100; g_fs_dl.err[0] = 0; } else snprintf(g_fs_dl.err, sizeof g_fs_dl.err, "%s", err);
    bsdr_mutex_unlock(g_fs_lock);
    if (ok) BSDR_INFO("bsdr.model", "face-swap models ready");
    else    BSDR_WARN("bsdr.model", "face-swap model download failed (%s)", err);
}
int bsdr_faceswap_download_start(void) {
    fs_lock_init();
    if (!g_fs_lock) return -1;
    bsdr_mutex_lock(g_fs_lock);
    if (g_fs_dl.active)          { bsdr_mutex_unlock(g_fs_lock); return 0; }
    if (bsdr_faceswap_models_ready()) { bsdr_mutex_unlock(g_fs_lock); return 0; }
    memset(&g_fs_dl, 0, sizeof g_fs_dl);
    g_fs_dl.active = 1; g_fs_dl.pct = -1;
    bsdr_mutex_unlock(g_fs_lock);
    if (!bsdr_thread_start_detached(fs_worker, NULL)) {
        bsdr_mutex_lock(g_fs_lock); g_fs_dl.active = 0;
        snprintf(g_fs_dl.err, sizeof g_fs_dl.err, "cannot start download thread"); bsdr_mutex_unlock(g_fs_lock);
        return -1;
    }
    return 0;
}
void bsdr_faceswap_download_state(bsdr_model_dl *out) {
    if (!out) return;
    fs_lock_init();
    if (!g_fs_lock) { memset(out, 0, sizeof *out); return; }
    bsdr_mutex_lock(g_fs_lock);
    *out = g_fs_dl;
    bsdr_mutex_unlock(g_fs_lock);
}
int bsdr_faceswap_import_zip(const char *zip_path) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zip_path, 0)) {
        BSDR_ERROR("bsdr.model", "cannot open faceswap zip %s", zip_path); return -1;
    }
    char dir[900]; bsdr_faceswap_model_dir(dir, sizeof dir);
    int imported = 0;
    for (int i = 0; i < BSDR_FACESWAP_NFILES; i++) {
        const char *want = bsdr_faceswap_files[i];
        int idx = mz_zip_reader_locate_file(&z, want, NULL, 0);
        if (idx < 0) { char alt[64]; snprintf(alt, sizeof alt, "buffalo_l/%s", want);
                       idx = mz_zip_reader_locate_file(&z, alt, NULL, 0); }
        if (idx < 0) continue;
        char dest[1200]; snprintf(dest, sizeof dest, "%s%c%s", dir, PATHSEP, want);
        if (mz_zip_reader_extract_to_file(&z, idx, dest, 0)) { imported++;
            BSDR_INFO("bsdr.model", "imported %s -> %s", want, dest); }
    }
    mz_zip_reader_end(&z);
    if (!imported) BSDR_WARN("bsdr.model", "no faceswap models found in %s", zip_path);
    return imported ? imported : -1;
}
