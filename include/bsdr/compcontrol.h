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
#include "bsdr/acl.h"
#include <stddef.h>
#include <stdint.h>

typedef struct bsdr_compcontrol bsdr_compcontrol;
typedef struct bsdr_app bsdr_app;

/* `inj` is borrowed (the local desktop injector). */
bsdr_compcontrol *bsdr_compcontrol_new(bsdr_injector *inj);
void bsdr_compcontrol_free(bsdr_compcontrol *cc);

/* Reusable input actuators (type a string, press a key combo like "ctrl+shift+t", click/scroll at
 * absolute coords) — exposed so the host can offer them as plugin services (bot-computer-control). */
void bsdr_cc_type(bsdr_injector *inj, const char *text);
void bsdr_cc_key(bsdr_injector *inj, const char *combo);
void bsdr_cc_click(bsdr_injector *inj, double x, double y, const char *button);
void bsdr_cc_scroll(bsdr_injector *inj, int amount);

/* Wire the "speak" tool to a TTS sink (e.g. bsdr_app_tts_say with user = the app). Optional; when
 * unset the model's speak calls report "speech not configured". */
void bsdr_compcontrol_set_speak(bsdr_compcontrol *cc, void (*cb)(void *, const char *), void *user);

/* Give the executor the app so the higher-tier tools (bot control, admin, web, moderation) can act.
 * Without it only the desktop/computer tools + speak work. */
void bsdr_compcontrol_set_app(bsdr_compcontrol *cc, bsdr_app *app);

/* The running command's abort flag, so the stop_talking tool can end the current turn. Optional. */
void bsdr_compcontrol_set_abort(bsdr_compcontrol *cc, volatile int *abort);

/* Set the CALLER context for the tool calls that follow: the allowed tool-group mask (BSDR_TG_*),
 * the speaker's level, and their display name. exec() rejects any tool outside the mask (defense in
 * depth) and uses the level/name for owner-only tools + admin actions. Set before each LLM run. */
void bsdr_compcontrol_set_caller(bsdr_compcontrol *cc, uint32_t mask, bsdr_acl_level level,
                                 const char *speaker);

/* The owner's full tool mask (respecting the app's global group toggles); BSDR_TG_ALL if no app.
 * Used by the owner's in-VR balloon to advertise exactly the enabled tools. */
uint32_t bsdr_compcontrol_owner_mask(bsdr_compcontrol *cc);

/* bsdr_llm_tool_cb-compatible: `user` is the bsdr_compcontrol*. */
void bsdr_compcontrol_exec(const char *name, const char *args_json,
                           char *result, size_t result_len, void *user);

#endif /* BSDR_COMPCONTROL_H */
