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
 *
 * PLUGIN EXCEPTION. As a special exception under section 7 of the GPL, a separate
 * plugin that is loaded by bsdrX at run time and interacts with it ONLY through the
 * interface declared in this header (the Plugin API) may be conveyed under license
 * terms of your choice, including proprietary terms, and does not thereby become
 * subject to the GPL. Including this header and building a plugin against it is
 * expressly permitted. The exception covers such plugins only; bsdrX itself, and any
 * plugin that uses more of bsdrX than this header, remain under the GPL. See the full
 * terms and conditions in COPYING.PLUGIN-EXCEPTION.
 */
/* bsdrX loadable plugin ABI.
 *
 * A plugin is a shared object exporting a single symbol, bsdr_plugin_register(), that returns a
 * const bsdr_plugin descriptor. At startup the agent scans the plugin directory (see plugin.c for the
 * search path; override with $BSDR_PLUGIN_DIR), dlopen()s each *.so, calls bsdr_plugin_register(),
 * checks the ABI, and runs init(). Plugins are unloaded in reverse order at shutdown.
 *
 * A plugin needs NO symbols from the host binary: everything it may call back into the host for is a
 * function pointer in the bsdr_plugin_host table handed to init(). This keeps the host free of
 * --export-dynamic and lets a plugin be built independently.
 *
 * This mechanism is public. Individual plugins carry a marking (private|closed|open; see
 * plugins/PLUGIN-AUTHORING.md): the build drops any plugins/<name>/ present into build/plugins/, and
 * scripts/publish-github.sh publishes ONLY 'open' plugins to the GitHub mirror while 'closed' and
 * 'private' ones stay on the private origin — so a proprietary plugin can ship (and be sold via the
 * store) without ever reaching the public mirror. */
#ifndef BSDR_PLUGIN_H
#define BSDR_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

/* ABI compatibility.
 *
 * BSDR_PLUGIN_ABI is the CURRENT ABI the host is built against; bump it by 1 whenever this contract
 * changes. BSDR_PLUGIN_ABI_MIN is the host's own hard floor — the oldest plugin build it will still
 * structurally load (raise it only to deliberately drop very old plugins).
 *
 * COMPATIBILITY IS A PER-PLUGIN RANGE. Each plugin declares the host ABIs it supports:
 *   - `abi`     = N = the MINIMUM host ABI it requires (the ABI it was built against). The host must
 *                 be at least this new, since a plugin built against ABI N expects those hooks.
 *   - `abi_max` = X = the LAST host ABI it is known compatible with. 0 (the default) means UNBOUNDED:
 *                 the plugin stays compatible with every host ABI >= N — retroactively, forever —
 *                 UNTIL the author caps it (sets X) to obsolete it for newer hosts.
 * The host (current ABI H) loads a plugin iff:  BSDR_PLUGIN_ABI_MIN <= N <= H  AND  (X == 0 || H <= X)
 * and REFUSES (never calls) anything outside that range — so a plugin that needs a newer host, or one
 * the author capped below this host, is skipped with a warning, never run.
 *
 * How to evolve this struct without breaking old plugins:
 *   1. Only ever APPEND new members to the end of bsdr_plugin / bsdr_plugin_host (never insert or
 *      reorder), and bump BSDR_PLUGIN_ABI.
 *   2. A plugin fills in `struct_size = sizeof(bsdr_plugin)` at ITS compile time, so a plugin built
 *      against an older, smaller struct reports a smaller size. The host uses BSDR_PLUGIN_HAS()
 *      before touching any hook, so it never reads past the bytes the plugin actually provided —
 *      that is what stops an older plugin from crashing a newer host (and vice-versa).
 *   3. Because abi_max defaults to 0 (unbounded), bumping BSDR_PLUGIN_ABI keeps every existing plugin
 *      loadable (their N stays <= the new H and their X stays open) — that is the "retroactive until
 *      marked obsolete" guarantee. Only capping a plugin's abi_max, or raising BSDR_PLUGIN_ABI_MIN,
 *      drops an old plugin. */
#define BSDR_PLUGIN_ABI     6   /* current host ABI (bump on every change to this header's contract) */
#define BSDR_PLUGIN_ABI_MIN 1   /* host's hard floor: oldest plugin build it will still load */

/* True if `field` lies wholly within the struct the plugin actually provided (per its struct_size),
 * i.e. safe for the host to read. Use it before calling any optional hook:
 *     if (BSDR_PLUGIN_HAS(p, http) && p->http) p->http(...);  */
#define BSDR_PLUGIN_HAS(p, field) \
    ((p)->struct_size >= offsetof(bsdr_plugin, field) + sizeof(((const bsdr_plugin *)0)->field))

/* A configuration variable a plugin declares (bsdr_plugin.config). The host auto-renders a form for
 * these in the plugin's panel/section, persists edited values, and the plugin reads them back live via
 * host->config_get. All fields are string literals owned by the plugin. */
typedef struct bsdr_plugin_cfgvar {
    const char *key;    /* stable id, persisted under the plugin's namespace (e.g. "api_url") */
    const char *label;  /* human label shown in the form */
    const char *type;   /* "text" (default) | "password" | "bool" | "number" */
    const char *def;    /* default value as a string ("" / "0" / "1" / …) */
    const char *help;   /* optional one-line hint (may be NULL) */
} bsdr_plugin_cfgvar;

/* Permission groups for registered tools — the `group` passed to host->tool_register and the mask used
 * with tool_list_json / tool_invoke. A bot plugin maps a caller's access level to a mask of these bits;
 * a tool is offered/invocable only when its group is within that mask. Public so independent plugins
 * (the base bot + moderation/translation/computer-control) agree on the values. Mirrors the host's
 * internal tool groups; keep in sync. */
enum {
    BSDR_PLUGIN_TG_PUBLIC    = 1u << 0,   /* chat, speak, web search — anyone the bot answers */
    BSDR_PLUGIN_TG_MODERATOR = 1u << 1,   /* kick, ban, mic-check */
    BSDR_PLUGIN_TG_BOTCTL    = 1u << 2,   /* follow, leave, stop, restart */
    BSDR_PLUGIN_TG_COMPUTER  = 1u << 3,   /* shell/files/input/screenshot */
    BSDR_PLUGIN_TG_ADMIN     = 1u << 4,   /* authorize/deauthorize */
    BSDR_PLUGIN_TG_BROWSER   = 1u << 5,   /* CDP browser control (owner-only, opt-in) */
    BSDR_PLUGIN_TG_TRANSLATE = 1u << 6,   /* translation (its own group, independent of moderator) */
};

/* A tool a plugin registers into the bot's LLM loop via host->tool_register (ABI 4). The host keeps the
 * registry so plugins compose WITHOUT a plugin->plugin ABI: the bot plugin (fullbot) enumerates tools by
 * the caller's permission level and dispatches each invocation back to the owning plugin's handler.
 * Given the model's JSON `args` and the caller's level, write a JSON result into out (<= cap) and return
 * 1 on success, 0 on failure. `state` is the owner plugin's own state pointer (what it passed at
 * registration). The handler must not retain `args`/`out` beyond the call. */
typedef int (*bsdr_tool_fn)(void *state, const char *args_json, int caller_level,
                            char *out, size_t cap);

/* A voice/audio effect (host->audio_fx_register, ABI 4): transform mic/voice PCM IN PLACE — 16-bit
 * interleaved at `rate` Hz, `channels` interleaved. Called on the audio path (once per frame); keep it
 * real-time and handle your own thread-safety. Lets a plugin OWN the voice changer (DSP / AI voice
 * conversion). While registered, the core routes the owner's voice through you instead of its built-in
 * effect. `user` is what you passed to audio_fx_register. */
typedef void (*bsdr_audio_fx_fn)(void *user, int16_t *pcm, int frames, int rate, int channels);

/* A SAME-DIMENSIONS video effect (host->video_fx_register, ABI 4): transform an NV12 frame IN PLACE — Y
 * plane (`y`, `y_stride` bytes/row) + interleaved UV plane (`uv`, `uv_stride`), `width` x `height`. Called
 * on the encode path in registration ORDER (lower `order` first), so effects chain (e.g. faceswap before
 * others). Keep it real-time. `user` is what you passed to video_fx_register. NB: this is for effects
 * that DON'T change frame size; a frame-reshaping effect (2D->3D SBS) is a separate, later mechanism. */
typedef void (*bsdr_video_fx_fn)(void *user, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                                 int width, int height);

/* A DIM-CHANGING video-source transform (host->video_src_register, ABI 4): the 2D->3D mechanism. Unlike
 * the same-dims chain, this RESHAPES the frame (2D -> side-by-side stereo), so it can't be a mere
 * per-frame fn — the plugin must declare its OUTPUT size at capture setup (the core sizes the encoder to
 * it), then transform src -> dst each frame. Single registrant (the 2D->3D plugin), engaged only while
 * the user has 3D enabled. The core passes ONLY sizes and caller-owned NV12 buffers, never a
 * plugin-owned handle, so the plugin keeps its own per-dimensions state and an unload can't strand a
 * dangling pointer in the core; the core re-opens the capture (renegotiating dims) on register/clear.
 *   dims():  given the source (in_w x in_h), fill the SBS output (out_w x out_h); return 1 to claim this
 *            source (the core sizes the encoder + allocates buffers to it) or 0 to decline (built-in 2D->3D
 *            or plain 2D handles it). Must be pure/stateless — called at every capture (re)open.
 *   apply(): transform one NV12 frame src(in_w x in_h) -> dst(out_w x out_h), all planes caller-owned. */
typedef struct bsdr_video_src_fx {
    int  (*dims)(void *user, int in_w, int in_h, int *out_w, int *out_h);
    void (*apply)(void *user, const uint8_t *sy, int sys, const uint8_t *suv, int suvs, int in_w, int in_h,
                  uint8_t *dy, int dys, uint8_t *duv, int duvs, int out_w, int out_h);
    void *user;
} bsdr_video_src_fx;

/* A DEPTH estimator (host->depth_fx_register, ABI 4): estimate depth for a `w`x`h` grayscale image
 * (row-major, `w` bytes/row) into `out` (w*h floats, 0..1, near=1). `tier` picks the model/quality
 * (1 cpu / 2 gpu / 3 hi). Return 0 on success, -1 on failure. This is the split-out inference the 2d-3d
 * plugin also exposes so a HOST that does its OWN side-by-side rendering (e.g. the Android GL SBS shader)
 * can source depth from the plugin instead of a built-in engine, while the desktop uses the full
 * video-source DIBR transform above. The plugin owns/reopens its engine on a tier change. */
typedef int (*bsdr_depth_fx_fn)(void *user, int tier, const uint8_t *gray, int w, int h, float *out);

/* A face-swap RGB interface (host->face_fx_register, ABI 4): for a host that already has packed-RGB
 * frames (e.g. Android's GL readback), instead of the NV12 video-fx chain the desktop encode path uses.
 * process() swaps faces in the packed-RGB (R,G,B rows) frame IN PLACE (returns faces swapped >=0, -1 if
 * not ready); set_source() sets the identity from a packed-RGB source image (0 ok, -1 fail). The faceswap
 * plugin registers this ALONGSIDE its NV12 video-fx so it runs on both desktop and Android. */
typedef struct bsdr_face_fx {
    int (*process)(void *user, uint8_t *rgb, int w, int h);
    int (*set_source)(void *user, const uint8_t *rgb, int w, int h);
    void *user;
} bsdr_face_fx;

/* A per-speaker audio-gain policy (host->audio_gain_policy, ABI 4). Fill up to `cap` {ssrc,gain} pairs
 * into ssrc_out/gain_out and the gain for everyone-else into *default_out; return the count, or -1 to
 * apply NO policy (leave the mix at unity). The host's room-audio consumer calls this periodically while
 * your plugin owns the policy — so a bot plugin can set "owner loud, strangers muted" from its own ACL.
 * `user` is what you passed to audio_gain_policy. Must be quick (no blocking); runs on the audio loop. */
typedef int (*bsdr_gain_policy_fn)(void *user, uint32_t *ssrc_out, float *gain_out, int cap, float *default_out);

/* Called when the operator selects (active=1) or leaves (active=0) a bot presence mode this plugin
 * registered via host->bot_mode_register (ABI 4). Also called with 0 just before the plugin unloads if
 * its mode was active. `user` is what you passed at registration. Bring your behaviour up/down here
 * (e.g. subscribe to utterances + enable the avatar on activate; the reverse on deactivate). */
typedef void (*bsdr_bot_mode_cb)(int active, void *user);

/* Delivered by host->utterance_subscribe (ABI 4): one COMPLETE room utterance (the host already ran
 * per-speaker VAD to segment it), with the speaker's SSRC and 48 kHz PCM. Called on a host worker
 * thread — so you may run STT / the LLM loop right here — never the audio thread. `user` is what you
 * passed to utterance_subscribe. Resolve the speaker with host->resolve_speaker(ssrc). */
typedef void (*bsdr_utterance_cb)(uint32_t ssrc, const int16_t *pcm, int frames, int channels, void *user);

/* ---- ABI 5: plugin-to-plugin services -----------------------------------------------------------
 * Plugins are dlopen'd RTLD_LOCAL, so one can never call another's symbols, and the tool registry only
 * carries JSON — no way to hand over PCM. A plugin that wants to offer something to the plugins that
 * DEPEND on it publishes a named interface here; they look it up by name. The host only brokers the
 * pointer, it never interprets it. Publish from init(); the host drops your services when you unload,
 * so a dependent must not cache the pointer across an unload.
 *
 * Only meaningful between a plugin and its declared deps: dependency order is what guarantees the
 * publisher's init() ran first. Name a service "<plugin>.<thing>". */
typedef void (*bsdr_hearing_cb)(uint32_t ssrc, const int16_t *pcm, int frames, int channels,
                                const char *transcript, void *user);

/* "fullbot.hearing" — the in-room bot's ear, re-published to its feature plugins. The bot's hearing is
 * single-owner (the host's utterance_subscribe REPLACES the callback), so a feature plugin must NOT
 * subscribe to the host directly: it would silently steal hearing from the bot. It asks the bot instead.
 * `transcript` is the one the bot ALREADY ran, so a subscriber never pays for a second STT; `pcm` is the
 * same utterance, for anything acoustic (levels, voice models). Both are owned by the caller and valid
 * only for the duration of the call. Delivered on the bot's audio worker: do not block. */
#define BSDR_SERVICE_HEARING "fullbot.hearing"
typedef struct {
    void *self;
    int  (*subscribe)(void *self, bsdr_hearing_cb cb, void *user);   /* 1 = subscribed */
    void (*unsubscribe)(void *self, void *user);
} bsdr_hearing_service;

/* "fullbot.audio" — the bot's per-speaker gain policy, steerable by its feature plugins. The host lets
 * exactly ONE owner set the gain policy and fullbot takes it while its mode is active, so a feature
 * plugin must not call host->audio_gain_policy itself: it would evict the bot's own levels. It asks the
 * bot to steer instead.
 *
 * The BASE is fullbot's per-level policy — the "most open" gain each speaker's level grants (owner full,
 * friends/hosts audible, strangers muted). On top of that base, features request a temporary FOCUS: bring
 * one speaker up and duck the rest, then restore the base when done. Because several features may steer at
 * once (a mic check on a stranger, a future host/friend spotlight), a focus carries a PRIORITY and the
 * highest active one wins — so they compose instead of clobbering each other. The tiers, low→high:
 *
 *   BSDR_FOCUS_MICCHECK  a mic/age check raising the stranger it is verifying
 *   BSDR_FOCUS_FRIEND    a friend-level spotlight
 *   BSDR_FOCUS_HOST      a host-level spotlight
 *   BSDR_FOCUS_OWNER     the owner — always wins
 *
 * The OWNER is never ducked by ANY focus (the operator must always be able to talk over whatever the bot
 * is doing in their own room), so an owner-tier focus is really about ducking everyone ELSE onto the owner.
 *
 * focus_at(ssrc, priority): set (ssrc != 0) or clear (ssrc == 0) THIS priority's request. The effective
 *   focus is the highest-priority request currently set; clearing one falls back to the next-highest, or
 *   to the base when none remain. A caller clears its own tier — never a tier it did not set.
 * focus(ssrc): shorthand for focus_at(ssrc, BSDR_FOCUS_MICCHECK) — the original single-tier call.
 * Both are safe to call from any thread. */
#define BSDR_SERVICE_ROOM_AUDIO "fullbot.audio"
enum {
    BSDR_FOCUS_MICCHECK = 10,
    BSDR_FOCUS_FRIEND   = 20,
    BSDR_FOCUS_HOST     = 30,
    BSDR_FOCUS_OWNER    = 40,
};
typedef struct {
    void *self;
    void (*focus)(void *self, uint32_t ssrc);                    /* = focus_at(ssrc, BSDR_FOCUS_MICCHECK) */
    /* --- appended in ABI 6: priority-arbitrated focus (see above) --- */
    void (*focus_at)(void *self, uint32_t ssrc, int priority);
} bsdr_room_audio_service;

/* Host services handed to a plugin at init. All are function pointers so a plugin links against
 * nothing in the host binary. APPEND-ONLY: never insert or reorder members (see the ABI note above);
 * a plugin may read only up to host->struct_size bytes. */
typedef struct bsdr_plugin_host {
    int    abi;                /* = BSDR_PLUGIN_ABI (a plugin may refuse a mismatch) */
    size_t struct_size;        /* = sizeof(bsdr_plugin_host); a plugin must not read beyond it */
    void  *app;                /* opaque bsdr_app* — pass back to host calls that want it */
    /* Leveled logger: level 0=debug 1=info 2=warn 3=error. */
    void (*log)(int level, const char *tag, const char *fmt, ...);
    /* Answer an HTTP request handed to your http() hook. conn is the opaque handle you were given. */
    void (*http_respond)(void *conn, int code, const char *ctype, const char *body, size_t len);
    /* Parse a JSON number out of a request body: returns 1 and sets *out on success, else 0. */
    int  (*json_get_double)(const char *body, const char *key, double *out);
    /* Per-user config dir (XDG/HOME/APPDATA), so a plugin can persist its own small state file.
     * Returns 1 and writes an absolute path into buf on success, else 0. */
    int  (*config_dir)(char *buf, size_t cap);
    /* --- appended in ABI 2 (guard with host->struct_size before use) ------------------------- */
    /* Parse a JSON string value out of a request body: returns 1 and writes into out, else 0. */
    int  (*json_get_str)(const char *body, const char *key, char *out, size_t cap);
    /* Namespaced string config the host persists for you (namespace = your plugin name). config_get
     * writes the stored value (or "" if unset) into out and returns 1 if a value existed. config_set
     * persists val and returns 1 on success. Use these for the variables you declared in
     * bsdr_plugin.config, or any ad-hoc key; values are readable live (no reload needed). */
    int  (*config_get)(const char *plugin, const char *key, char *out, size_t cap);
    int  (*config_set)(const char *plugin, const char *key, const char *val);
    /* --- appended in ABI 4: bot host-service surface (see PLAN-bot-plugin.md) ------------------- */
    /* Tool registry — the mechanism the bot plugins compose through. `group` is a required-permission
     * bitmask (use the bot's BSDR_TG_* values); `schema_json` is the JSON-schema of the tool's
     * parameters, handed to the model. Re-registering the same name replaces it. Returns 1 on success.
     * When your plugin is unloaded the host automatically drops every tool it registered (keyed by the
     * owner_state pointer), so you need not unregister in shutdown. */
    int    (*tool_register)(const char *name, const char *description, const char *schema_json,
                            unsigned group, bsdr_tool_fn fn, void *owner_state);
    /* Remove one registered tool by name (optional; unload cleans up for you). */
    void   (*tool_unregister)(const char *name);
    /* Append the model-facing tool-list JSON array for every tool whose group is within allowed_mask,
     * into out; returns bytes written. The bot plugin uses this to build a caller's toolset. */
    size_t (*tool_list_json)(unsigned allowed_mask, char *out, size_t cap);
    /* Invoke a registered tool. Re-checks that caller_group_mask permits the tool's group (defence in
     * depth) before dispatching to its handler. Returns 1 and fills out on success; 0 if unknown/denied. */
    int    (*tool_invoke)(const char *name, const char *args_json, int caller_level,
                          unsigned caller_group_mask, char *out, size_t cap);

    /* Room / identity (see PLAN-bot-plugin.md §5). A bot plugin polls these each tick (there is no
     * change callback yet — poll roster_json; it's cheap). */
    /* Append the current room roster as a JSON array — [{"ssrc","username","socialId","legacyUserId",
     * "isHost","isSelf"},…] — into out; returns bytes written ("[]" if empty). */
    size_t   (*roster_json)(char *out, size_t cap);
    /* SSRC of the room owner (0 if not identified). */
    uint32_t (*owner_ssrc)(void);
    /* Resolve a speaker's SSRC to {"socialId","username","level","isHost","isSelf"} (level 0=none,
     * 1=friend, 2=host, 3=owner). Returns 1 and fills out_json if the SSRC is in the roster, else 0. */
    int      (*resolve_speaker)(uint32_t ssrc, char *out_json, size_t cap);

    /* Cloud / moderation actions (the moderator plugin wraps these as registered tools). Each resolves
     * the target by display name via the roster. kick/ban return 1 on success; reset_room returns the
     * number of participants kicked. ban also persists the socialId so a rejoin is auto-re-kicked. */
    int      (*kick_user)(const char *username);
    int      (*ban_user)(const char *username);
    int      (*reset_room)(void);

    /* Desktop: capture a JPEG screenshot (max_dim caps the longest side; 0 = full). Writes into out,
     * returns the byte length, or 0 on failure. Used by the computer-control plugin's vision tool. */
    int      (*screenshot_jpeg)(int max_dim, unsigned char *out, size_t cap);

    /* Speech. `speak` enqueues text for the bot to say into the room (non-blocking; TTS -> the bot's
     * cloud room-mic producer). `stt` transcribes PCM using the agent's configured STT endpoint,
     * writing text into out; returns 1 on success, 0 on failure. */
    void     (*speak)(const char *text);
    int      (*stt)(const int16_t *pcm, int frames, int rate, int channels, char *out, size_t cap);

    /* Avatar / presence. `avatar_state` is the bot's presence state (0=off 1=connecting 2=up 3=ghost).
     * `local_legacy_user_id` writes the bot's own cloud-assigned legacyUserId into out; returns 1 if
     * known. `avatar_set_follow` toggles follow-me (the bot re-joins whatever room the operator moves
     * into); `avatar_follow` reads the current setting. The avatar wire serialization (a flatbuffer) is
     * a host actuator — the plugin controls behaviour, the core does the encoding + transport. */
    int      (*avatar_state)(void);
    int      (*local_legacy_user_id)(char *out, size_t cap);
    void     (*avatar_set_follow)(int on);
    int      (*avatar_follow)(void);

    /* LLM: one model round-trip for a plugin-owned agentic loop (loop-in-fullbot). POSTs a full messages
     * JSON array + tools JSON array to the agent's configured endpoint and writes the assistant `message`
     * object JSON (content + tool_calls) into out; returns 1 on success, 0 on failure. The PLUGIN owns
     * the loop (parse tool_calls, run them via tool_invoke, append results, call again). tools_json
     * NULL/"" => no tools. */
    int      (*llm_complete)(const char *messages_json, const char *tools_json, char *out, size_t cap);

    /* Escape a string for embedding inside JSON you build (chat messages, tool results). Writes into out
     * (a bare escaped string, no surrounding quotes); returns bytes that would be written (snprintf
     * semantics). Pairs with json_get_str/json_get_double for a plugin that assembles LLM requests. */
    int      (*json_escape)(char *out, size_t cap, const char *s);

    /* Room hearing. Subscribe to complete room utterances: the host runs per-speaker VAD on the incoming
     * audio and calls cb(ssrc, pcm, frames, channels, user) once per finished utterance, on a host
     * worker thread (run STT / the LLM loop there). One subscriber — a second call replaces it, NULL
     * disables. While a plugin is subscribed the in-core command router is bypassed, so the bot's
     * hearing/decisions are entirely the plugin's. The host clears the subscription when the plugin
     * unloads. */
    void     (*utterance_subscribe)(bsdr_utterance_cb cb, void *user);

    /* ABI 5: publish / look up a named plugin-to-plugin interface (see bsdr_hearing_service above).
     * service_publish returns 1 on success; service_get returns NULL when nobody published that name —
     * always check, the publisher may be disabled or not installed. */
    int      (*service_publish)(const char *name, void *iface);
    void    *(*service_get)(const char *name);

    /* Bot presence MODE. The core always offers the bare "audio" mode (REST join + room audio only — no
     * avatar, no brain). A plugin registers additional modes (e.g. "fullbot") that IMPLEMENT everything
     * above bare audio; the operator picks one and the host calls your on_active(1/0) as it's selected/
     * left. Register from init(); the host drops your modes (calling on_active(0) if active) on unload.
     * bot_mode_get reports the current selection. avatar_enable turns the core's avatar actuator on/off —
     * a plugin mode calls it when it activates (the wire serialization stays in the core; see §5). */
    void     (*bot_mode_register)(const char *name, bsdr_bot_mode_cb on_active, void *user);
    void     (*bot_mode_get)(char *out, size_t cap);
    void     (*avatar_enable)(int on);

    /* Register/clear (NULL) a per-speaker audio-gain policy. While set, the core applies YOUR policy to
     * the room-audio mix (see bsdr_gain_policy_fn); with none, the bare core leaves the mix at unity and
     * silences no one. The host clears it when your plugin unloads. */
    void     (*audio_gain_policy)(bsdr_gain_policy_fn fn, void *user);

    /* Desktop input (computer-control). Drive the host desktop: type a string, press a key combo
     * ("ctrl+shift+t"), click/scroll at absolute pixel coords. For a computer-control plugin whose tools
     * are owner-only. (Screenshot capture is screenshot_jpeg above; shell/file ops a plugin can do with
     * libc directly.) */
    void     (*input_type)(const char *text);
    void     (*input_key)(const char *keys);
    void     (*input_click)(double x, double y, const char *button);
    void     (*input_scroll)(int amount);

    /* Register/clear (NULL) a voice/audio effect (bsdr_audio_fx_fn). While set, the core routes the
     * owner's mic/voice PCM through your effect instead of its built-in one — this is how a voice-changer
     * plugin owns the DSP + AI voice conversion. Cleared when your plugin unloads. */
    void     (*audio_fx_register)(bsdr_audio_fx_fn fn, void *user);

    /* Model store — the shared model download/cache engine stays in the CORE (used by depth, faceswap
     * and voice). A model-using plugin resolves READY model paths from it, so it never links httpc or
     * duplicates the downloader. All fill `out` and return 1 on success (0 = not present / error).
     * `model_dir` = the shared cache root. The voice_* serve the RVC voice library:
     * `voice_base_path(which,…)` which = 0 content-encoder, 1 rmvpe(pitch); `voice_base_ready` = the
     * base models are downloaded; `voice_list_json` = [{"id","name","sr"},…] of installed voices. */
    int      (*model_dir)(char *out, size_t cap);
    int      (*voice_path)(const char *id, char *out, size_t cap);
    int      (*voice_base_path)(int which, char *out, size_t cap);
    int      (*voice_base_ready)(void);
    size_t   (*voice_list_json)(char *out, size_t cap);

    /* Add/remove a same-dimensions video effect to the encode-path chain (bsdr_video_fx_fn). Effects run
     * in ascending `order`. Call with fn=NULL to remove your entry (keyed by `user`). The host also
     * removes your effects when the plugin unloads. For a face-swap plugin. */
    void     (*video_fx_register)(bsdr_video_fx_fn fn, void *user, int order);

    /* Register (or clear, with NULL) the single dim-changing video-source transform (bsdr_video_src_fx*).
     * The struct is COPIED by value, so it may be a stack local. While set (and the user has 3D on) the
     * core sizes the encoder from your dims() and routes each frame through your apply() instead of the
     * built-in 2D->3D. The core re-opens the capture on register/clear so the encoder is re-sized, and
     * clears it automatically when your plugin unloads. For the 2D->3D plugin. */
    void     (*video_src_register)(const bsdr_video_src_fx *fx);

    /* Read the current face-swap config the core UI owns (the effect stays wired to the core's model
     * store + model-manager UI; a plugin owns only the ONNX PROCESSING). Any out-pointer may be NULL.
     * `on` = effect enabled, `tier` = 0..3 (>=2 asks for a GPU EP), `source` = path to the source face
     * image, `detect_every` = detect-cadence (>=2 = every N frames). For the face-swap plugin. */
    void     (*faceswap_config)(int *on, int *tier, char *source, size_t source_cap, int *detect_every);

    /* Decode an image file to packed RGB (R,G,B rows), via the core's FFmpeg image decoder. On success
     * fills *rgb (malloc'd — the caller frees it with free()) + *w + *h and returns 0; -1 on failure.
     * Generic (any image-consuming plugin); the face-swap plugin uses it to load the source face. */
    int      (*decode_image_rgb)(const char *path, uint8_t **rgb, int *w, int *h);

    /* Read the current 2D->3D config the core UI owns (the effect stays wired to the core's config +
     * model store + model-manager UI; the plugin owns the depth + SBS synthesis). Any out-pointer may be
     * NULL. `mode` 0 off/1 fast/2 ai, `deepness` 0..100, `convergence` -50..50, `swap`/`full` flags,
     * `tier` 0 ext/1 cpu/2 gpu/3 hi, `ai_cmd` = external depth-helper command. For the 2d-3d plugin. */
    void     (*threed_config)(int *mode, int *deepness, int *convergence, int *swap, int *full, int *tier,
                              char *ai_cmd, size_t ai_cap);

    /* Depth-model provider (the core model store stays in the core, shared): per-tier preprocessing
     * params + cached-file resolution + background download. The 2d-3d plugin wraps these into a
     * bsdr_depth_provider so its bundled depth engine never links the model store. tier = 1 cpu/2 gpu/3 hi.
     * `depth_model_params` fills name/input_size/mean/std (return 0 ok); the other two mirror the store. */
    int      (*depth_model_params)(int tier, char *name, size_t name_cap, int *input_size, float mean[3], float std[3]);
    int      (*depth_model_resolve)(int tier, int allow_download, char *path, size_t cap);
    int      (*depth_model_download_start)(int tier);

    /* Register (or clear, with NULL) a depth estimator (bsdr_depth_fx_fn). A host that renders stereo
     * itself (Android's GL SBS) calls the core's bsdr_mediafx_apply_depth to source depth from the
     * plugin. Cleared automatically when the plugin unloads. For the 2d-3d plugin. */
    void     (*depth_fx_register)(bsdr_depth_fx_fn fn, void *user);

    /* Register (or clear, with NULL) the packed-RGB face-swap interface (bsdr_face_fx*, copied by value).
     * A host with RGB frames (Android's GL readback) calls bsdr_mediafx_face_process / _set_source.
     * Cleared automatically when the plugin unloads. For the face-swap plugin. */
    void     (*face_fx_register)(const bsdr_face_fx *fx);
} bsdr_plugin_host;

/* What a plugin provides. abi + struct_size + name are required; every hook may be NULL.
 * APPEND-ONLY: add new members only at the end and bump BSDR_PLUGIN_ABI (see the ABI note above). */
typedef struct bsdr_plugin {
    int abi;                   /* N: MINIMUM host ABI required (set to BSDR_PLUGIN_ABI at build time) */
    size_t struct_size;        /* set to sizeof(bsdr_plugin) so the host knows which hooks you have */
    const char *name;          /* stable id, e.g. "legacy-mic"; used in the /api/plugin/<name>/ prefix */
    const char *version;
    const char *description;

    /* Lifecycle. init returns 0 on success (nonzero => the plugin is dropped). *state is yours. */
    int  (*init)(const bsdr_plugin_host *host, void **state);
    void (*shutdown)(void *state);

    /* Handle a request whose path starts with /api/plugin/<name>/. Return 1 if handled (you must have
     * called host->http_respond), 0 to let the host continue matching / 404. */
    int  (*http)(void *state, const char *method, const char *path, const char *body, void *conn);

    /* Append a self-contained HTML fragment for the bot card. It is regenerated on every status poll,
     * so it can reflect live state; wire buttons with inline onclick="api('/api/plugin/<name>/..',{..})"
     * (the panel exposes a global api(path,body) helper). Write up to cap bytes; set *len to how many. */
    void (*ui_html)(void *state, char *buf, size_t cap, size_t *len);

    /* Override the bot-mic keepalive cadence: return a period in ms > 0 to override the host default,
     * or 0 to leave it. The first plugin returning >0 wins. */
    int  (*mic_keepalive_period_ms)(void *state);

    /* X: the LAST host ABI this plugin is compatible with. 0 (default, if you leave it unset) means
     * unbounded — compatible with every host ABI >= abi, retroactively, until you cap it. Set it to a
     * specific ABI only to obsolete this plugin for hosts newer than that. Appended in ABI 2; the host
     * reads it via BSDR_PLUGIN_HAS(p, abi_max) and treats an absent field as 0 (unbounded). */
    int abi_max;

    /* A TOP-LEVEL panel of your own (its own card in the web UI), distinct from ui_html which injects
     * a fragment into the bot card. Regenerated on every status poll so it can show live state; it may
     * carry its own <style>/<script>, and reference assets/APIs you serve from your http() hook
     * (e.g. src="/api/plugin/<name>/app.js"). Write up to cap bytes, set *len.
     * Emit the card's CONTENTS starting with your <h2> — the host wraps them in the <div class=card>.
     * Wrapping your own nests a card in a card: doubled chrome, and the <h2> ends up where the collapse
     * rule hides it with the body, so the panel can't be reopened. Title only in the <h2> (no
     * "(<name> plugin)" suffix); the host adds the collapse chevron and the state badge. */
    void (*panel_html)(void *state, char *buf, size_t cap, size_t *len);

    /* Declared configuration variables. The host auto-renders a form for them in your panel section,
     * persists edited values, and you read them back live with host->config_get(name,key,..). Point
     * `config` at a static array of `config_count` entries; leave NULL/0 if you have none. */
    const bsdr_plugin_cfgvar *config;
    int config_count;

    /* Add sections INTO EXISTING panels (not your own card). Emit one or more fragments each tagged
     * with a target slot, e.g.  "<div data-slot=\"headset\">…</div><div data-slot=\"source\">…</div>".
     * The page routes each fragment into the card slot of that name (a fragment for an unknown slot is
     * appended to the plugin-panels area). Regenerated every poll. Slots are pure HTML (no <script>);
     * for behaviour, use ui_script below. */
    void (*sections_html)(void *state, char *buf, size_t cap, size_t *len);

    /* A URL to a JavaScript asset the host loads ONCE as a real <script> (so, unlike HTML injected via
     * innerHTML, it actually executes). Serve it from your http() hook, e.g.
     * ui_script = "/api/plugin/<name>/ui.js". The script gets full DOM access plus a small documented
     * `window.bsdrUI` API (bsdrUI.onStatus(cb) to run on every poll, bsdrUI.slot(name), bsdrUI.api()) —
     * that is how a plugin can ADD, MODIFY, or REMOVE any section/panel/element in the page. NULL = none.
     * See docs/PLUGIN-ABI.md. */
    const char *ui_script;

    /* --- appended in ABI 3: plugin dependencies ---------------------------------------------- */
    /* Other plugins this one needs in order to work. Point `deps` at a static array of `dep_count`
     * plugin `name` ids (e.g. (const char*[]){"legacy-mic"}); leave NULL/0 if you have none.
     *
     * The host enforces them at LOAD time: this plugin is init()'d only after every named dependency
     * has itself loaded and init'd successfully, and it is loaded BEFORE it (so your init may rely on a
     * dependency already being up). If a dependency is missing, disabled, incompatible, fails init, or
     * forms a cycle, THIS plugin is skipped with a warning and never run — a hard requirement, not a
     * hint. Names refer to a plugin's descriptor `name`, not its file/slug (usually identical).
     *
     * The plugin store mirrors this: when you install a plugin it first fetches and installs any
     * missing dependencies, so a plugin "can't be installed without the ones it needs". Keep this list
     * in sync with DEPENDS in plugins/<name>/store.conf (the store's copy, used at install time). */
    const char *const *deps;
    int dep_count;
} bsdr_plugin;

/* The one symbol every plugin shared object must export. */
const bsdr_plugin *bsdr_plugin_register(void);

/* ---- host-side manager (called by the agent; not by plugins) -------------------------------- */

/* Load every plugin found in the search path and init it against `app`. Idempotent-ish: safe to call
 * once at startup. */
void bsdr_plugins_load(void *app);
/* Shut down + dlclose all loaded plugins (reverse order). */
void bsdr_plugins_unload(void);
/* Unload then rescan (against the app from the last load), honoring the disabled-list — used after a
 * store download or an enable/disable toggle so it takes effect without restarting the agent. */
void bsdr_plugins_reload(void);
/* Number of successfully loaded plugins. */
int  bsdr_plugins_count(void);
/* 1 if a plugin with this descriptor name is currently loaded. */
int  bsdr_plugins_is_loaded(const char *name);
/* The per-user, always-writable plugin dir (<config_dir>/plugins), created on demand. The store client
 * installs downloaded plugins here and the loader always scans it. Returns 1 on success. */
int  bsdr_plugins_user_dir(char *out, size_t cap);

/* Route an HTTP request to a plugin. Returns 1 if a plugin handled it (and responded), else 0. */
int  bsdr_plugins_http(const char *method, const char *path, const char *body, void *conn);
/* Append `["name",..]` (JSON array) of the currently-loaded plugins' descriptor names. The web UI gates
 * plugin-backed feature cards on this. Returns bytes written. */
size_t bsdr_plugins_names_json(char *out, size_t cap);
/* Append `[{"name","loaded","enabled","builtin","version"},..]` (JSON array) of EVERY plugin present on
 * disk — app/dev dir (builtin) and per-user store dir — with its live state, for the web UI's installed-
 * plugins list + live enable/disable. Returns bytes written. */
size_t bsdr_plugins_installed_json(char *out, size_t cap);
/* Append `[{"name":..,"html":..},..]` (JSON array, HTML fields escaped) of every plugin exposing a
 * ui_html hook. Returns bytes written (0 and "[]" not written if none — caller emits the key). */
size_t bsdr_plugins_ui_json(char *out, size_t cap);
/* Same, for plugins exposing panel_html — full top-level cards rendered separately from the bot card. */
size_t bsdr_plugins_panel_json(char *out, size_t cap);
/* Same, for plugins exposing sections_html — fragments the page routes into named card slots. */
size_t bsdr_plugins_sections_json(char *out, size_t cap);
/* Append `[{"name":..,"src":..},..]` of plugins declaring a ui_script, so the page loads each once as
 * a real <script>. Returns bytes written. */
size_t bsdr_plugins_scripts_json(char *out, size_t cap);
/* Append `[{"plugin":..,"vars":[{"key":..,"label":..,"type":..,"value":..,"help":..}],..}]` for every
 * plugin declaring config variables; `value` is the persisted value or the declared default. */
size_t bsdr_plugins_config_json(char *out, size_t cap);
/* Persist one declared config variable for a loaded plugin. Returns 1 on success, 0 if the plugin or
 * key isn't a declared variable (so the web endpoint can't write arbitrary namespaced keys). */
int    bsdr_plugins_config_set(const char *plugin, const char *key, const char *val);
/* Resolve the bot-mic keepalive cadence: the first plugin override, or `def` if none. */
int  bsdr_plugins_mic_keepalive_period_ms(int def);

#endif /* BSDR_PLUGIN_H */
