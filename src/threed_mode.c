/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* 2D->3D / depth glue that stays in the CORE after the synthesis engine (threed.c + depth_onnx.c) moved
 * to the 2d-3d plugin: the tiny mode/tier <-> string helpers the CLI + config need (agent.c's main()
 * parses --threed / --threed-tier), plus the process-wide --ort-arena-off flag. The core keeps the 3D
 * CONFIG, model store, and CPU-encode policy; the plugin owns the actual depth + SBS synthesis. */
#include "bsdr/threed.h"
#include "bsdr/depth.h"
#include <string.h>

bsdr_threed_mode bsdr_threed_mode_parse(const char *s) {
    if (!s) return BSDR_3D_OFF;
    if (!strcmp(s, "fast")) return BSDR_3D_FAST;
    if (!strcmp(s, "ai"))   return BSDR_3D_AI;
    return BSDR_3D_OFF;
}
const char *bsdr_threed_mode_name(bsdr_threed_mode m) {
    return m == BSDR_3D_FAST ? "fast" : m == BSDR_3D_AI ? "ai" : "off";
}

bsdr_depth_tier bsdr_depth_tier_parse(const char *s) {
    if (!s) return BSDR_DEPTH_AUTO;
    if (!strcmp(s, "cpu")) return BSDR_DEPTH_CPU;
    if (!strcmp(s, "gpu")) return BSDR_DEPTH_GPU;
    if (!strcmp(s, "hi"))  return BSDR_DEPTH_HI;
    return BSDR_DEPTH_AUTO;
}
const char *bsdr_depth_tier_name(bsdr_depth_tier t) {
    switch (t) { case BSDR_DEPTH_CPU: return "cpu"; case BSDR_DEPTH_GPU: return "gpu";
                 case BSDR_DEPTH_HI: return "hi"; default: return "auto"; }
}

/* --ort-arena-off (P4.6): disable ORT's CPU memory arena. Defined here (a core lib file, always
 * compiled) so agent.c can extern + set it from the CLI. NB: after the depth/faceswap engines moved to
 * plugins, the core no longer READS this — each plugin carries its own copy — so the flag is effectively
 * inert for those plugins now (an experimental knob; a host-config plumb-through could restore it). */
int bsdr_ort_arena_off = 0;
