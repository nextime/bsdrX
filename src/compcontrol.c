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
/* LLM computer-control: tool calls -> uinput actions / app launch. */
#include "bsdr/compcontrol.h"
#include "bsdr/events.h"
#include "bsdr/json.h"
#include "bsdr/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

struct bsdr_compcontrol { bsdr_injector *inj; };

bsdr_compcontrol *bsdr_compcontrol_new(bsdr_injector *inj) {
    bsdr_compcontrol *cc = calloc(1, sizeof(*cc));
    if (cc) cc->inj = inj;
    return cc;
}
void bsdr_compcontrol_free(bsdr_compcontrol *cc) { free(cc); }

static void key_event(bsdr_injector *inj, uint16_t vk, bool down) {
    bsdr_input_event e;
    e.kind = BSDR_EV_KEY; e.u.key.is_vk = true; e.u.key.value = vk; e.u.key.down = down;
    bsdr_injector_handle(inj, &e);
}
static void char_event(bsdr_injector *inj, uint16_t ch, bool down) {
    bsdr_input_event e;
    e.kind = BSDR_EV_KEY; e.u.key.is_vk = false; e.u.key.value = ch; e.u.key.down = down;
    bsdr_injector_handle(inj, &e);
}

/* Windows VK codes (the injector maps VK -> native). */
static uint16_t name_to_vk(const char *n) {
    if (strlen(n) == 1) {
        char c = (char)toupper((unsigned char)n[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (uint16_t)c;
    }
    if (!strcasecmp(n, "ctrl") || !strcasecmp(n, "control")) return 0x11;
    if (!strcasecmp(n, "alt")) return 0x12;
    if (!strcasecmp(n, "shift")) return 0x10;
    if (!strcasecmp(n, "super") || !strcasecmp(n, "meta") || !strcasecmp(n, "win")) return 0x5B;
    if (!strcasecmp(n, "enter") || !strcasecmp(n, "return")) return 0x0D;
    if (!strcasecmp(n, "tab")) return 0x09;
    if (!strcasecmp(n, "esc") || !strcasecmp(n, "escape")) return 0x1B;
    if (!strcasecmp(n, "space")) return 0x20;
    if (!strcasecmp(n, "backspace")) return 0x08;
    if (!strcasecmp(n, "delete") || !strcasecmp(n, "del")) return 0x2E;
    if (!strcasecmp(n, "up")) return 0x26;
    if (!strcasecmp(n, "down")) return 0x28;
    if (!strcasecmp(n, "left")) return 0x25;
    if (!strcasecmp(n, "right")) return 0x27;
    if (!strcasecmp(n, "home")) return 0x24;
    if (!strcasecmp(n, "end")) return 0x23;
    return 0;
}

static void do_type(bsdr_injector *inj, const char *text) {
    for (const char *p = text; *p; p++) {
        char_event(inj, (uint8_t)*p, true);
        char_event(inj, (uint8_t)*p, false);
    }
}

/* "ctrl+shift+t" -> modifiers held around the final key */
static void do_key(bsdr_injector *inj, const char *combo) {
    char buf[128]; snprintf(buf, sizeof(buf), "%s", combo);
    uint16_t mods[4]; int nm = 0; uint16_t key = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        while (*tok == ' ') tok++;
        uint16_t vk = name_to_vk(tok);
        if (vk == 0x11 || vk == 0x12 || vk == 0x10 || vk == 0x5B) {
            if (nm < 4) mods[nm++] = vk;
        } else {
            key = vk;
        }
    }
    for (int i = 0; i < nm; i++) key_event(inj, mods[i], true);
    if (key) { key_event(inj, key, true); key_event(inj, key, false); }
    for (int i = nm - 1; i >= 0; i--) key_event(inj, mods[i], false);
}

static void do_click(bsdr_injector *inj, double x, double y, const char *button) {
    bsdr_input_event mv;
    mv.kind = BSDR_EV_MOVE_ABS; mv.u.move_abs.x = x; mv.u.move_abs.y = y;
    bsdr_injector_handle(inj, &mv);
    bsdr_mouse_button b = BSDR_BTN_LEFT;
    if (button && !strcasecmp(button, "right")) b = BSDR_BTN_RIGHT;
    else if (button && !strcasecmp(button, "middle")) b = BSDR_BTN_MIDDLE;
    bsdr_input_event d; d.kind = BSDR_EV_BUTTON; d.u.button.button = b; d.u.button.down = true;
    bsdr_injector_handle(inj, &d);
    d.u.button.down = false; bsdr_injector_handle(inj, &d);
}

static void do_scroll(bsdr_injector *inj, int amount) {
    bsdr_input_event e; e.kind = BSDR_EV_SCROLL; e.u.scroll.dx = 0; e.u.scroll.dy = amount;
    bsdr_injector_handle(inj, &e);
}

static void do_open(const char *name, char *result, size_t rl) {
    /* allow only a plain command word + simple args; launch detached */
    for (const char *p = name; *p; p++)
        if (strchr(";|&`$<>(){}\n", *p)) { snprintf(result, rl, "rejected"); return; }
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "setsid %s >/dev/null 2>&1 &", name);
    int rc = system(cmd);
    snprintf(result, rl, rc == 0 ? "launched" : "launch failed");
}

void bsdr_compcontrol_exec(const char *name, const char *args, char *result,
                           size_t rl, void *user) {
    bsdr_compcontrol *cc = (bsdr_compcontrol *)user;
    snprintf(result, rl, "ok");
    if (!strcmp(name, "type_text")) {
        char text[2048] = "";
        bsdr_json_get_str(args, "text", text, sizeof(text));
        do_type(cc->inj, text);
    } else if (!strcmp(name, "key")) {
        char keys[128] = "";
        bsdr_json_get_str(args, "keys", keys, sizeof(keys));
        do_key(cc->inj, keys);
    } else if (!strcmp(name, "click")) {
        double x = 0.5, y = 0.5; char btn[16] = "left";
        bsdr_json_get_double(args, "x", &x);
        bsdr_json_get_double(args, "y", &y);
        bsdr_json_get_str(args, "button", btn, sizeof(btn));
        do_click(cc->inj, x, y, btn);
    } else if (!strcmp(name, "scroll")) {
        double a = 0; bsdr_json_get_double(args, "amount", &a);
        do_scroll(cc->inj, (int)a);
    } else if (!strcmp(name, "open_app")) {
        char app[128] = "";
        bsdr_json_get_str(args, "name", app, sizeof(app));
        do_open(app, result, rl);
    } else {
        snprintf(result, rl, "unknown tool");
    }
    BSDR_INFO("bsdr.cc", "exec %s -> %s", name, result);
}
