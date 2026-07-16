/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* Per-speaker utterance router for the in-room bot.
 *
 * Fed by the audio receiver's per-SSRC tap (audible speakers only — owner/host/friend, per the
 * volume policy), it runs a lightweight VAD per speaker; when an utterance completes it is queued to
 * a single worker (one at a time). The worker acts ONLY on a speaker the owner has armed — a
 * one-shot translation or a mic-check; every other utterance is dropped without even transcribing.
 *
 * It is deliberately NOT a command router: nobody addresses this bot by name. Conversing with the
 * room — wake word, personality, tools for friends — is the fullbot plugin's job, and when fullbot
 * is loaded it takes the hearing outright and this tap never runs at all. The core's own control
 * surface is the owner's in-VR balloon (see voice.c). */
#ifndef BSDR_ROOMCMD_H
#define BSDR_ROOMCMD_H

#include <stdint.h>

typedef struct bsdr_app bsdr_app;
typedef struct bsdr_roomcmd bsdr_roomcmd;

/* A snapshot of the router's live state for the web UI status panel. */
typedef struct {
    int  enabled;              /* utterance processing on */
    int  queued;              /* utterances waiting in the worker queue */
    int  overloaded;          /* backlog over threshold => owner-only right now */
    int  mic_check_active;    /* a mic-check (age verify) is in progress */
    char mic_check_user[64];  /* who it targets */
    int  translate_active;    /* a one-shot translation is armed */
    char translate_lang[32];  /* target language */
} bsdr_roomcmd_status;

/* Create the router. Returns NULL if out of memory. */
bsdr_roomcmd *bsdr_roomcmd_new(bsdr_app *app);
void bsdr_roomcmd_free(bsdr_roomcmd *rc);

/* Enable/disable processing. When disabled the tap drops audio (no VAD, no dispatch), so the
 * router can be kept alive for the process lifetime — avoiding a free-vs-audio-thread race — while the
 * owner toggles the assistant on and off. Disabled by default until the agent turns it on. */
void bsdr_roomcmd_set_enabled(bsdr_roomcmd *rc, int on);

/* Tell the router which SSRC is the room owner (0 = unknown), for overload protection: when the
 * backlog exceeds a threshold the router serves only the owner until it drains. Pushed
 * periodically (no lock inversion) since resolving it needs the roster + ACL. */
void bsdr_roomcmd_set_owner_ssrc(bsdr_roomcmd *rc, uint32_t ssrc);

/* Snapshot the router's live state (for the web UI status panel). Safe to call from any thread. */
void bsdr_roomcmd_state(bsdr_roomcmd *rc, bsdr_roomcmd_status *st);

/* Arm one-shot translation: the NEXT utterance from `target_ssrc` is transcribed, translated into
 * `to_language`, and spoken to the room (no wake word needed) — then the arm clears. The caller
 * should also boost that speaker (app.mic_check_ssrc) so it's audible + tapped. */
void bsdr_roomcmd_arm_translate(bsdr_roomcmd *rc, uint32_t target_ssrc, const char *to_language);

/* Arm a mic-check (age verification) on `target_ssrc`: the bot prompts them to speak; their next
 * utterance is judged, and if they seem under 18 — or don't respond within ~20 s — they are kicked
 * and banned. The caller should boost that speaker (app.mic_check_ssrc). One-shot. */
void bsdr_roomcmd_arm_miccheck(bsdr_roomcmd *rc, uint32_t target_ssrc, const char *username);

/* Auto mic-check an unknown joiner: arms a mic-check ONLY if no check/translation is already running
 * and this SSRC hasn't been auto-checked this session (dedup) — and boosts the target itself. Returns
 * 1 if it armed one. Safe to call for every unknown participant on each roster refresh; it self-limits
 * to one at a time. */
int bsdr_roomcmd_autocheck(bsdr_roomcmd *rc, uint32_t target_ssrc, const char *username);

/* bsdr_ssrc_pcm_cb-compatible audio tap: `user` is the bsdr_roomcmd*. Runs per-speaker VAD and, on a
 * completed utterance, queues it for transcription + dispatch. Fast + non-blocking (audio thread). */
void bsdr_roomcmd_tap(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user);

#endif /* BSDR_ROOMCMD_H */
