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
/* Exercise the STT + LLM clients against (mock or real) endpoints.
 *   voice_test <stt_url> <llm_url> [token]
 */
#include "bsdr/stt.h"
#include "bsdr/llm.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void tool_cb(const char *name, const char *args, char *result,
                    size_t rl, void *user) {
    (*(int *)user)++;
    printf("  [tool] %s(%s)\n", name, args);
    snprintf(result, rl, "ok");
}

int main(int argc, char **argv) {
    bsdr_log_set_level(BSDR_LOG_INFO);
    bsdr_platform_init();
    const char *stt_url = argc > 1 ? argv[1] : "";
    const char *llm_url = argc > 2 ? argv[2] : "";
    const char *token = argc > 3 ? argv[3] : "";
    int fail = 0;

    if (stt_url[0]) {
        bsdr_stt_config sc; memset(&sc, 0, sizeof(sc));
        snprintf(sc.endpoint, sizeof(sc.endpoint), "%s", stt_url);
        snprintf(sc.token, sizeof(sc.token), "%s", token);
        snprintf(sc.model, sizeof(sc.model), "whisper-1");
        static int16_t pcm[48000 * 2];                 /* 1 s 48k stereo tone */
        for (int i = 0; i < 48000; i++) {
            int16_t v = (int16_t)(8000 * sin(2 * M_PI * 440 * i / 48000.0));
            pcm[2*i] = v; pcm[2*i+1] = v;
        }
        char text[1024] = "";
        if (bsdr_stt_transcribe(&sc, pcm, 48000, 48000, 2, text, sizeof(text)))
            printf("STT -> \"%s\"\n", text);
        else { printf("STT FAILED\n"); fail++; }
    }

    if (llm_url[0]) {
        bsdr_llm_config lc; memset(&lc, 0, sizeof(lc));
        snprintf(lc.endpoint, sizeof(lc.endpoint), "%s", llm_url);
        snprintf(lc.token, sizeof(lc.token), "%s", token);
        snprintf(lc.model, sizeof(lc.model), "test");
        int tools = 0; char reply[1024] = "";
        if (bsdr_llm_run(&lc, NULL, "open the web browser", tool_cb, &tools, reply, sizeof(reply)))
            printf("LLM: %d tool call(s), reply=\"%s\"\n", tools, reply);
        else { printf("LLM FAILED\n"); fail++; }
        if (tools < 1) { printf("LLM: expected a tool call\n"); fail++; }
    }

    printf(fail ? "\nFAILED\n" : "\nOK - STT + LLM clients work\n");
    return fail ? 1 : 0;
}
