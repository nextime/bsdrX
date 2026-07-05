/* test_depth.c — end-to-end check of the in-process ONNX depth tier.
 * Downloads (or uses the cached) tier-1 model, runs inference on a synthetic gradient, and asserts
 * the output is a sane 0..1 grid. Skips cleanly if ONNX isn't compiled in. GPL v3.
 */
#include "bsdr/depth.h"
#include "bsdr/model_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (!bsdr_depth_available()) { printf("SKIP: built without BSDR_HAVE_ONNX\n"); return 0; }

    bsdr_depth_tier tier = argc > 1 ? bsdr_depth_tier_parse(argv[1]) : BSDR_DEPTH_CPU;
    if (tier == BSDR_DEPTH_AUTO) tier = BSDR_DEPTH_CPU;
    bsdr_depth *d = bsdr_depth_open(tier);
    if (!d) { printf("SKIP: tier %s model unavailable (no network / import a model zip)\n", bsdr_depth_tier_name(tier)); return 0; }
    printf("engine: %s\n", bsdr_depth_status(d));

    int w = 128, h = 72;
    unsigned char *g = malloc((size_t)w * h);
    float *out = malloc((size_t)w * h * sizeof(float));
    if (!g || !out) return 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) g[y * w + x] = (unsigned char)(x * 255 / (w - 1)); /* L->R ramp */

    if (bsdr_depth_infer(d, g, w, h, out) != 0) { printf("FAIL: infer\n"); return 1; }

    float mn = 1e9f, mx = -1e9f, avg = 0;
    for (int i = 0; i < w * h; i++) { float v = out[i]; if (v < mn) mn = v; if (v > mx) mx = v; avg += v; }
    avg /= (float)(w * h);
    printf("depth range [%.3f, %.3f] avg %.3f\n", mn, mx, avg);
    if (mn < -0.01f || mx > 1.01f || mx <= mn) { printf("FAIL: output not a normalized grid\n"); return 1; }

    bsdr_depth_close(d); free(g); free(out);
    printf("PASS\n");
    return 0;
}
