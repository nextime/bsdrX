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
/* OpenAI-compatible LLM client with tool-calling, for the voice assistant.
 * POST <endpoint> {model, messages, tools}; executes returned tool calls via a
 * callback (computer control) and loops until the model returns a final reply.
 * http/https + optional bearer token. */
#ifndef BSDR_LLM_H
#define BSDR_LLM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char endpoint[256];   /* e.g. https://api.openai.com/v1/chat/completions */
    char token[256];      /* bearer token */
    char model[64];       /* e.g. "gpt-4o-mini" */
    /* Agentic loop + context management (0 => sane defaults are applied at run time):
     *   context_tokens  the model's context window; 0 => auto-detect from the endpoint, else 32768
     *   compact_pct     compact the conversation once it passes this % of the window (0 => 80; <0 off)
     *   compact_strategy BSDR_COMPACT_* (truncate / summary / hybrid)
     *   max_rounds      tool-call rounds before giving up (0 => 24) — a coding task needs many      */
    int context_tokens;
    int compact_pct;
    int compact_strategy;
    int max_rounds;
} bsdr_llm_config;

/* Best-effort: ask the endpoint's /models for this model's context window (context_length /
 * max_context_length / context_window). Returns tokens, or 0 if it can't be determined. Network call;
 * cache the result. */
int bsdr_llm_detect_context(const bsdr_llm_config *cfg);

/* Single model round-trip for a plugin-owned agentic loop (see PLAN-bot-plugin.md, loop-in-fullbot).
 * POSTs one request from a full messages array + tools array and returns the assistant `message` object
 * JSON (content + tool_calls) in out. The caller owns the loop (parse tool_calls, run them, re-call).
 * Returns 1 on success, else 0. tools_json NULL/"" => no tools. */
int bsdr_llm_complete_once(const bsdr_llm_config *cfg, const char *messages_json,
                           const char *tools_json, char *out, size_t cap);

/* Execute one tool call; write a short result string. */
typedef void (*bsdr_llm_tool_cb)(const char *name, const char *args_json,
                                 char *result, size_t result_len, void *user);

/* Provide a desktop screenshot (JPEG) on demand into `out`; return byte count
 * (0 = none). Given to bsdr_llm_run_ex to expose a `take_screenshot` tool the
 * model can call when the task needs it to see the screen (vision models). */
typedef int (*bsdr_llm_image_cb)(void *user, uint8_t *out, size_t cap);

/* Run the assistant: send `user_text` with the computer-control tools, execute
 * any tool calls via `cb`, loop until a final reply. Final text -> `out`. */
bool bsdr_llm_run(const bsdr_llm_config *cfg, const char *system_prompt,
                  const char *user_text, bsdr_llm_tool_cb cb, void *user,
                  char *out, size_t out_len);

/* As bsdr_llm_run, but the offered tools are filtered by `toolmask` (a bitmask of BSDR_TG_* tool
 * groups from bsdr/acl.h) so a caller advertises only the tools the speaker's access level allows.
 * BSDR_TG_ALL offers everything (bsdr_llm_run passes this). If `img_cb` is non-NULL AND the mask
 * includes the computer group, a `take_screenshot` tool is offered; when the model calls it the
 * screenshot is captured and attached as an image the model sees next turn (on-demand vision, needs
 * a vision-capable model). `abort` (optional) is polled between rounds and tool calls; when it
 * becomes non-zero the run stops promptly (the in-VR stop balloon / stop_talking). */
bool bsdr_llm_run_ex(const bsdr_llm_config *cfg, const char *system_prompt,
                     const char *user_text, bsdr_llm_tool_cb cb, void *user,
                     bsdr_llm_image_cb img_cb, void *img_user, uint32_t toolmask,
                     const volatile int *abort, char *out, size_t out_len);

/* The tool group (a BSDR_TG_* bit) a tool name belongs to, or 0 if unknown. Lets the executor
 * re-check that a called tool is within the caller's allowed mask (defense-in-depth vs a model that
 * invents or is coaxed into a tool above the speaker's level). */
uint32_t bsdr_llm_tool_group(const char *name);

#endif /* BSDR_LLM_H */
