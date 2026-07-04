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
/* Computer-control tool executor for the LLM: maps tool calls (type_text, key,
 * click, scroll, open_app) to real desktop actions via the uinput injector and
 * app launching. Plugs into bsdr_llm_run as the tool callback. */
#ifndef BSDR_COMPCONTROL_H
#define BSDR_COMPCONTROL_H

#include "bsdr/inject.h"
#include <stddef.h>

typedef struct bsdr_compcontrol bsdr_compcontrol;

/* `inj` is borrowed (the local desktop injector). */
bsdr_compcontrol *bsdr_compcontrol_new(bsdr_injector *inj);
void bsdr_compcontrol_free(bsdr_compcontrol *cc);

/* bsdr_llm_tool_cb-compatible: `user` is the bsdr_compcontrol*. */
void bsdr_compcontrol_exec(const char *name, const char *args_json,
                           char *result, size_t result_len, void *user);

#endif /* BSDR_COMPCONTROL_H */
