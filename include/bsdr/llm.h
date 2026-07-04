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
} bsdr_llm_config;

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

/* As bsdr_llm_run, but if `img_cb` is non-NULL a `take_screenshot` tool is offered;
 * when the model calls it the screenshot is captured and attached as an image the
 * model sees on the next turn (on-demand vision). Use a vision-capable model.
 * `abort` (optional) is polled between rounds and tool calls; when it becomes
 * non-zero the run stops promptly (used by the in-VR stop balloon). */
bool bsdr_llm_run_ex(const bsdr_llm_config *cfg, const char *system_prompt,
                     const char *user_text, bsdr_llm_tool_cb cb, void *user,
                     bsdr_llm_image_cb img_cb, void *img_user,
                     const volatile int *abort, char *out, size_t out_len);

#endif /* BSDR_LLM_H */
