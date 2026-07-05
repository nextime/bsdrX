/* threed.c — real-time 2D->3D (side-by-side) synthesis. See threed.h. GPL v3.
 *
 * Pipeline per frame (NV12, in place):
 *   1. copy the source planes to a scratch buffer (we read from it while overwriting the frame);
 *   2. (re)estimate a small depth grid — FAST heuristic, or the AI helper every few frames;
 *   3. warp: for each output column, sample the source shifted horizontally by the local depth,
 *      squished 2:1 into the left / right half of the frame (the two eye views).
 * Nearest sampling and a tiny depth grid keep it to a few linear passes over the frame.
 */
#include "bsdr/threed.h"
#include "bsdr/depth.h"
#include "bsdr/model_store.h"
#include "bsdr/log.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#define DEPTH_W   256      /* depth-estimate working width (height derived from aspect) */
#define AI_EVERY  3        /* AI mode: recompute depth every N frames, reuse in between */

/* Bilinear luma sample at fractional column fx (row already selected). Clamped to [0,n-1]. The 2:1
 * horizontal squish samples at the 2-pixel-group centre, so bilinear here doubles as the downscale
 * anti-alias filter (nearest sampling looked jaggy). */
static inline uint8_t lerp1(const uint8_t *row, float fx, int n) {
    if (fx < 0) fx = 0; else if (fx > n - 1) fx = (float)(n - 1);
    int x0 = (int)fx; float f = fx - x0; int x1 = x0 + 1; if (x1 >= n) x1 = n - 1;
    return (uint8_t)(row[x0] * (1.0f - f) + row[x1] * f + 0.5f);
}
/* Bilinear sample of one interleaved U/V pair (chroma), writing both bytes to out[0],out[1]. */
static inline void lerp2(const uint8_t *row, float fx, int npairs, uint8_t *out) {
    if (fx < 0) fx = 0; else if (fx > npairs - 1) fx = (float)(npairs - 1);
    int x0 = (int)fx; float f = fx - x0; int x1 = x0 + 1; if (x1 >= npairs) x1 = npairs - 1;
    out[0] = (uint8_t)(row[x0 * 2]     * (1.0f - f) + row[x1 * 2]     * f + 0.5f);
    out[1] = (uint8_t)(row[x0 * 2 + 1] * (1.0f - f) + row[x1 * 2 + 1] * f + 0.5f);
}

struct bsdr_threed {
    bsdr_threed_config cfg;
    int sw, sh;       /* source dims */
    int out_w;        /* packed SBS output width (2*sw if full, else sw) */

    /* depth working buffers, dw x dh, values 0(far)..1(near) */
    int dw, dh;
    uint8_t *gray;   /* downscaled luma fed to the estimator */
    float   *depth;  /* current depth grid (temporally smoothed) */
    float   *dnew;   /* freshly estimated grid before smoothing */
    uint64_t frames;

    /* in-process neural depth (depth.h / ONNX Runtime), cross-platform. tier 0 = use the
     * co-process/heuristic; 1/2/3 = CPU/GPU/HI. Opened lazily; falls back on failure. */
    int          tier;
    int          depth_gave_up;   /* 1 once we stopped trying the in-process engine this session */
    bsdr_depth  *engine;

#ifndef _WIN32
    /* AI co-process */
    pid_t   ai_pid;
    int     ai_in;    /* write frames here (child stdin) */
    int     ai_out;   /* read depth here (child stdout) */
    int     ai_dead;  /* 1 once we gave up on it -> heuristic */
#endif
};

bsdr_threed_mode bsdr_threed_mode_parse(const char *s) {
    if (!s) return BSDR_3D_OFF;
    if (!strcmp(s, "fast")) return BSDR_3D_FAST;
    if (!strcmp(s, "ai"))   return BSDR_3D_AI;
    return BSDR_3D_OFF;
}
const char *bsdr_threed_mode_name(bsdr_threed_mode m) {
    return m == BSDR_3D_FAST ? "fast" : m == BSDR_3D_AI ? "ai" : "off";
}

/* ---- AI helper co-process (POSIX) ------------------------------------------------------------ */
#ifndef _WIN32
static void ai_spawn(bsdr_threed *t) {
    if (!t->cfg.ai_cmd[0]) {
        BSDR_WARN("bsdr.threed", "AI mode selected but no depth-helper command set -> using the fast "
                  "heuristic. Configure one in the web panel (e.g. scripts/bsdr-depth-helper.py).");
        t->ai_dead = 1; return;
    }
    int to[2], from[2];
    if (pipe(to) != 0) { t->ai_dead = 1; return; }
    if (pipe(from) != 0) { close(to[0]); close(to[1]); t->ai_dead = 1; return; }
    pid_t pid = fork();
    if (pid < 0) { close(to[0]); close(to[1]); close(from[0]); close(from[1]); t->ai_dead = 1; return; }
    if (pid == 0) {                       /* child: stdin<-to, stdout->from, run the operator command */
        dup2(to[0], 0); dup2(from[1], 1);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", t->cfg.ai_cmd, (char *)NULL);
        _exit(127);
    }
    close(to[0]); close(from[1]);
    t->ai_pid = pid; t->ai_in = to[1]; t->ai_out = from[0];
    BSDR_INFO("bsdr.threed", "AI depth helper started: %s", t->cfg.ai_cmd);
}

/* write all n bytes; returns 0 on success, -1 on error (SIGPIPE ignored process-wide by the host) */
static int write_all(int fd, const uint8_t *b, size_t n) {
    while (n) { ssize_t r = write(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}
/* read exactly n bytes with a total deadline of ms; -1 on timeout/EOF/error */
static int read_all_to(int fd, uint8_t *b, size_t n, int ms) {
    while (n) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0) return -1;
        ssize_t r = read(fd, b, n);
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}

/* Ask the helper for a depth grid. Frame contract (both directions):
 *   ['B','S','D','D'][u16 w LE][u16 h LE][w*h gray bytes]
 * returns 0 and fills t->dnew (0..1) on success; -1 on any failure (caller falls back to FAST). */
static int ai_depth(bsdr_threed *t) {
    if (t->ai_dead) return -1;
    if (!t->ai_pid) { ai_spawn(t); if (t->ai_dead) return -1; }
    int dw = t->dw, dh = t->dh, np = dw * dh;
    uint8_t hdr[8] = { 'B','S','D','D',
                       (uint8_t)(dw & 0xff), (uint8_t)(dw >> 8),
                       (uint8_t)(dh & 0xff), (uint8_t)(dh >> 8) };
    uint8_t rh[8];
    if (write_all(t->ai_in, hdr, 8) != 0 || write_all(t->ai_in, t->gray, (size_t)np) != 0) goto dead;
    /* first response can be slow (model warm-up); later ones must be prompt or we skip the update */
    if (read_all_to(t->ai_out, rh, 8, t->frames < AI_EVERY ? 4000 : 250) != 0) goto dead;
    if (memcmp(rh, "BSDD", 4) != 0) goto dead;
    int rw = rh[4] | (rh[5] << 8), rht = rh[6] | (rh[7] << 8);
    if (rw != dw || rht != dh) goto dead;
    uint8_t *tmp = t->gray;    /* reuse gray as the receive buffer AFTER we've sent it */
    if (read_all_to(t->ai_out, tmp, (size_t)np, 500) != 0) goto dead;
    for (int i = 0; i < np; i++) t->dnew[i] = tmp[i] / 255.0f;
    return 0;
dead:
    BSDR_WARN("bsdr.threed", "AI depth helper unresponsive -> heuristic depth");
    t->ai_dead = 1;
    return -1;
}
static void ai_stop(bsdr_threed *t) {
    if (t->ai_pid) {
        close(t->ai_in); close(t->ai_out);
        kill(t->ai_pid, SIGTERM);
        waitpid(t->ai_pid, NULL, 0);
        t->ai_pid = 0;
    }
}
#endif /* !_WIN32 */

/* ---- depth estimation -------------------------------------------------------------------------*/
/* Downscale the source luma into t->gray (dw x dh, nearest). */
static void build_gray(bsdr_threed *t, const uint8_t *sy, int stride) {
    for (int gy = 0; gy < t->dh; gy++) {
        int syy = gy * t->sh / t->dh;
        const uint8_t *row = sy + (size_t)syy * stride;
        uint8_t *g = t->gray + (size_t)gy * t->dw;
        for (int gx = 0; gx < t->dw; gx++) g[gx] = row[gx * t->sw / t->dw];
    }
}
/* FAST heuristic: depth = vertical gradient (far top / near bottom) blended with per-frame
 * min-max-normalised luma (brighter reads as nearer). Comfortable and essentially free. */
static void heuristic_depth(bsdr_threed *t) {
    int np = t->dw * t->dh, lo = 255, hi = 0;
    for (int i = 0; i < np; i++) { int v = t->gray[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    float inv = hi > lo ? 1.0f / (hi - lo) : 0.0f;
    for (int gy = 0; gy < t->dh; gy++) {
        float grad = t->dh > 1 ? (float)gy / (t->dh - 1) : 0.5f;   /* 0 top(far) .. 1 bottom(near) */
        for (int gx = 0; gx < t->dw; gx++) {
            float ln = inv ? (t->gray[gy * t->dw + gx] - lo) * inv : 0.5f;
            t->dnew[gy * t->dw + gx] = 0.6f * grad + 0.4f * ln;
        }
    }
}

/* smooth a freshly estimated t->dnew into t->depth (damps per-estimate flicker) and return. */
static void smooth_into_depth(bsdr_threed *t) {
    int np = t->dw * t->dh;
    for (int i = 0; i < np; i++) t->depth[i] = 0.5f * t->depth[i] + 0.5f * t->dnew[i];
}

static void update_depth(bsdr_threed *t, const uint8_t *sy, int stride) {
    int ai = (t->cfg.mode == BSDR_3D_AI);
    /* Any AI source recomputes only every AI_EVERY frames and reuses the grid in between. */
    if (ai && (t->frames % AI_EVERY) != 0 && t->frames > 0) return;
    build_gray(t, sy, stride);

    /* 1) in-process neural depth (cross-platform), when a tier is selected. */
    if (ai && t->tier > 0 && !t->depth_gave_up && bsdr_depth_available()) {
        if (!t->engine) {
            t->engine = bsdr_depth_open((bsdr_depth_tier)t->tier);   /* kicks a bg download if uncached */
            if (!t->engine) {
                /* Only give up permanently if the model IS present but still won't load (no ORT / bad
                 * model). If it's merely absent, a background download is underway — keep retrying so
                 * neural depth kicks in the moment the download lands (heuristic covers the gap). */
                if (bsdr_model_present(t->tier)) {
                    t->depth_gave_up = 1;
                    BSDR_WARN("bsdr.threed", "in-process depth (tier %d) unavailable -> co-process/heuristic", t->tier);
                }
            }
            else BSDR_INFO("bsdr.threed", "in-process depth: %s", bsdr_depth_status(t->engine));
        }
        if (t->engine && bsdr_depth_infer(t->engine, t->gray, t->dw, t->dh, t->dnew) == 0) {
            smooth_into_depth(t); return;
        }
    }
#ifndef _WIN32
    /* 2) external depth co-process (POSIX only). */
    if (ai && !t->ai_dead && ai_depth(t) == 0) { smooth_into_depth(t); return; }
#endif
    /* 3) built-in heuristic. */
    heuristic_depth(t);
    memcpy(t->depth, t->dnew, (size_t)t->dw * t->dh * sizeof(float));
}

/* nearest depth lookup for source pixel (sx,sy) -> 0..1 */
static inline float depth_at(bsdr_threed *t, int sx, int sy) {
    int gx = sx * t->dw / t->sw, gy = sy * t->dh / t->sh;
    if (gx < 0) gx = 0; else if (gx >= t->dw) gx = t->dw - 1;
    if (gy < 0) gy = 0; else if (gy >= t->dh) gy = t->dh - 1;
    return t->depth[gy * t->dw + gx];
}

/* ---- public ----------------------------------------------------------------------------------*/
bsdr_threed *bsdr_threed_create(int src_w, int src_h, const bsdr_threed_config *cfg) {
    if (!cfg || cfg->mode == BSDR_3D_OFF || src_w < 16 || src_h < 16) return NULL;
    bsdr_threed *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cfg = *cfg;
    t->tier = cfg->tier;
    t->sw = src_w; t->sh = src_h;
    t->out_w = src_w;   /* half-SBS: packed output == source frame size (correct-aspect screen) */
    t->dw = src_w > DEPTH_W ? DEPTH_W : (src_w & ~1);
    t->dh = (int)((long)t->dw * src_h / src_w) & ~1;
    if (t->dh < 16) t->dh = 16;
    int np = t->dw * t->dh;
    t->gray  = malloc((size_t)np);
    t->depth = calloc((size_t)np, sizeof(float));
    t->dnew  = calloc((size_t)np, sizeof(float));
    if (!t->gray || !t->depth || !t->dnew) { bsdr_threed_close(t); return NULL; }
    for (int i = 0; i < np; i++) t->depth[i] = 0.5f;
    BSDR_INFO("bsdr.threed", "2D->3D SBS %dx%d (half-SBS) mode=%s deepness=%d conv=%d%s",
              src_w, src_h, bsdr_threed_mode_name(cfg->mode), cfg->deepness, cfg->convergence,
              cfg->swap ? " swap" : "");
    return t;
}

int bsdr_threed_out_width(bsdr_threed *t) { return t ? t->out_w : 0; }

void bsdr_threed_apply_nv12(bsdr_threed *t,
                            const uint8_t *sy, int sy_stride, const uint8_t *suv, int suv_stride,
                            uint8_t *dy, int dy_stride, uint8_t *duv, int duv_stride) {
    if (!t || t->cfg.mode == BSDR_3D_OFF) return;
    int sw = t->sw, sh = t->sh, ow = t->out_w;

    /* depth from the source */
    update_depth(t, sy, sy_stride);
    t->frames++;

    /* Parallax in SOURCE pixels; convergence moves the zero-parallax plane. Each output eye is the
     * source sampled with a per-pixel horizontal shift. scale = source cols per output eye col:
     * 1 for full-res (1:1), 2 for half-width (squished, sampled at the 2-px group centre so bilinear
     * also anti-aliases the downscale). */
    float amp = (t->cfg.deepness / 100.0f) * (sw * 0.04f);
    float c0  = 0.5f - t->cfg.convergence / 100.0f;
    int eye_w = ow / 2;                                 /* output luma cols per eye */
    float scale = (float)sw / eye_w;                    /* 1 (full) or 2 (half) */
    float half  = (scale - 1.0f) * 0.5f;                /* group-centre offset */
    int left_is_left = !t->cfg.swap;

    /* luma */
    for (int yy = 0; yy < sh; yy++) {
        const uint8_t *src = sy + (size_t)yy * sy_stride;
        uint8_t *dst = dy + (size_t)yy * dy_stride;
        for (int xo = 0; xo < eye_w; xo++) {
            float base = xo * scale + half;             /* source column for this eye pixel */
            float d = depth_at(t, (int)base, yy) - c0;
            float shift = amp * d * 0.5f;
            int lo = left_is_left ? xo : eye_w + xo;
            int ro = left_is_left ? eye_w + xo : xo;
            dst[lo] = lerp1(src, base + shift, sw);     /* left eye */
            dst[ro] = lerp1(src, base - shift, sw);     /* right eye */
        }
    }
    /* chroma (NV12 UV pairs); half the horizontal resolution of luma */
    int scw = sw / 2;                                   /* source chroma pairs per row */
    int eye_cw = eye_w / 2;                             /* output chroma pairs per eye */
    for (int yy = 0; yy < sh / 2; yy++) {
        const uint8_t *src = suv + (size_t)yy * suv_stride;
        uint8_t *dst = duv + (size_t)yy * duv_stride;
        int sy2 = yy * 2;
        for (int co = 0; co < eye_cw; co++) {
            float base = co * scale + half;             /* source chroma pair */
            float d = depth_at(t, (int)(base * 2), sy2) - c0;
            float shift = amp * d * 0.5f * 0.5f;
            int lo = left_is_left ? co : eye_cw + co;
            int ro = left_is_left ? eye_cw + co : co;
            lerp2(src, base + shift, scw, dst + lo * 2);
            lerp2(src, base - shift, scw, dst + ro * 2);
        }
    }
}

void bsdr_threed_close(bsdr_threed *t) {
    if (!t) return;
#ifndef _WIN32
    ai_stop(t);
#endif
    bsdr_depth_close(t->engine);
    free(t->gray); free(t->depth); free(t->dnew);
    free(t);
}
