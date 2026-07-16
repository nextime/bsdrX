# bsdrX plugin ABI (v6)

This is the complete, public reference for writing a bsdrX **loadable plugin** — a shared object
(`.so`/`.dll`) the agent `dlopen()`s at startup. The contract is the single header
[`include/bsdr/plugin.h`](../include/bsdr/plugin.h); this document explains it. A plugin needs **no**
symbols from the host binary: everything it may call back into the host is a function pointer handed to
it at init, so a plugin builds independently and links only libc + this header.

Because a plugin stays within this API it is covered by the **bsdrX Plugin Exception**
(`COPYING.PLUGIN-EXCEPTION`, a GPLv3 §7 additional permission) and may be licensed under terms of your
choice, including proprietary. See [PLUGIN-AUTHORING.md](../plugins/PLUGIN-AUTHORING.md) for the
packaging/marking/store checklist.

---

## 1. The one export

```c
#include "bsdr/plugin.h"
const bsdr_plugin *bsdr_plugin_register(void);   /* the only required symbol */
```

At startup the agent scans the plugin directory, `dlopen()`s each `*.so`/`.dll`, calls
`bsdr_plugin_register()`, checks the ABI (below), then runs `init()`. Plugins are unloaded in reverse
order at shutdown, and reloaded live when you install/enable/disable one from the web UI.

Search path (first existing wins): `$BSDR_PLUGIN_DIR`, `<exe>/plugins`, `<exe>/../lib/bsdrX/plugins`,
`<exe>/../share/bsdrX/plugins`, `./build/plugins`, and **always** `<config_dir>/plugins`
(`~/.config/bsdr_agent/plugins`) — the per-user dir where the in-app store installs downloads.

## 2. The descriptor

```c
static const bsdr_plugin MY_PLUGIN = {
    .abi         = BSDR_PLUGIN_ABI,     /* N: minimum host ABI you require (see §3) */
    .abi_max     = 0,                   /* X: last host ABI you support; 0 = unbounded (see §3) */
    .struct_size = sizeof(bsdr_plugin), /* REQUIRED: lets the host know which hooks you have */
    .name        = "my-plugin",         /* stable id; used in the /api/plugin/<name>/ URL prefix */
    .version     = "1.0.0",
    .description = "What it does",
    /* every hook below is optional (may be NULL) */
    .init = my_init, .shutdown = my_shutdown,
    .http = my_http, .ui_html = my_ui, .panel_html = my_panel,
    .sections_html = my_sections, .ui_script = "/api/plugin/my-plugin/ui.js",
    .config = MY_VARS, .config_count = 3,
    .deps = (const char*[]){ "other-plugin" }, .dep_count = 1,  /* §8: plugins you require */
    .mic_keepalive_period_ms = my_mic_period,
};
const bsdr_plugin *bsdr_plugin_register(void) { return &MY_PLUGIN; }
```

`abi`, `struct_size`, and `name` are required; everything else may be NULL/0. **Always set
`struct_size = sizeof(bsdr_plugin)`** — it is how a newer host safely runs an older plugin.

## 3. ABI compatibility — a per-plugin range `[N .. X]`

The host is built with two constants:

| constant              | meaning                                                              |
|-----------------------|---------------------------------------------------------------------|
| `BSDR_PLUGIN_ABI`     | the host's **current** ABI (H). Bumped whenever this header changes. |
| `BSDR_PLUGIN_ABI_MIN` | the host's **hard floor** — the oldest plugin build it will still load. |

Each **plugin** declares the host ABIs it supports:

- **`abi` = N** — the *minimum* host ABI you require (the `BSDR_PLUGIN_ABI` you compiled against). An
  older host that predates a hook you use can't run you, so it won't.
- **`abi_max` = X** — the *last* host ABI you are compatible with. **`0` (the default) means
  unbounded**: you stay compatible with every host ABI ≥ N, **retroactively, forever**, until you
  deliberately cap it. Set X to a specific ABI only to obsolete yourself for hosts newer than that.

The host loads a plugin **iff**:

```
BSDR_PLUGIN_ABI_MIN  <=  N  <=  H       and       (X == 0  ||  H <= X)
```

Anything outside that range is skipped with a warning, never called — a plugin that needs a newer host
(or one you capped below this host) can't crash it. This is the **retroactive-until-obsolete**
guarantee: bumping the host ABI keeps every existing plugin loadable (their N stays ≤ the new H, their
X stays open); only capping a plugin's `abi_max`, or the operator marking a *store version* obsolete,
retires an old build.

**How a newer host runs an older plugin safely.** When the header grows, new members are only ever
**appended** to `bsdr_plugin` / `bsdr_plugin_host`, and `BSDR_PLUGIN_ABI` is bumped. A plugin reports
its own `struct_size`, so the host uses `BSDR_PLUGIN_HAS(p, field)` before touching any optional hook
and never reads past the bytes the plugin actually provided. You do the same for host services you use
(check `host->struct_size`). Never insert or reorder members.

The store mirrors this: an artifact stores `abi` (=N) and `abi_max` (=X); the agent sends its single
current ABI H on download, and the store returns the newest non-obsolete version whose range contains
H. See §8.

## 4. Host services (`bsdr_plugin_host`)

Handed to `init(const bsdr_plugin_host *host, void **state)`. Guard anything past the original set with
`host->struct_size`.

| service | since | use |
|---|---|---|
| `log(level, tag, fmt, …)` | 1 | leveled logger (0=debug…3=error) |
| `http_respond(conn, code, ctype, body, len)` | 1 | answer a request in your `http` hook |
| `json_get_double(body, key, *out)` | 1 | parse a number from a request body |
| `config_dir(buf, cap)` | 1 | your per-user dir for ad-hoc state files |
| `json_get_str(body, key, out, cap)` | 2 | parse a string from a request body |
| `config_get(plugin, key, out, cap)` | 2 | read a persisted config value (namespace = your name) |
| `config_set(plugin, key, val)` | 2 | persist a config value |
| `tool_register` / `tool_unregister` / `tool_list_json` / `tool_invoke` | 4 | the bot's tool registry (§8) |
| `utterance_subscribe(cb, user)` | 4 | **take** the bot's hearing — single-owner; feature plugins want `fullbot.hearing` (§9) instead |
| `service_publish(name, iface)` / `service_get(name)` | 5 | publish / look up a plugin-to-plugin interface (§9) |

## 5. Backend: API endpoints + assets (`http` hook)

```c
int my_http(void *state, const char *method, const char *path, const char *body, void *conn);
```

The host routes every request under **`/api/plugin/<name>/…`** to your `http` hook. Return `1` if you
handled it (after calling `host->http_respond`), else `0`. This is how a plugin **registers an API
endpoint** *and* **serves assets** — respond with any content type:

```c
if (!strcmp(path, "/api/plugin/my-plugin/ui.js"))
    { host->http_respond(conn, 200, "application/javascript", JS, strlen(JS)); return 1; }
if (!strcmp(method,"POST") && !strcmp(path,"/api/plugin/my-plugin/do"))
    { /* … */ host->http_respond(conn, 200, "application/json", "{\"ok\":true}", 11); return 1; }
```

## 6. Front-end: four ways to touch the web UI

The panel polls `/api/status` ~1×/s; your HTML hooks are regenerated each poll so they can show live
state. A global `api(path, body)` helper (GET without body, POST with) is available to inline handlers.

1. **`ui_html`** — a fragment injected into the **bot card** (legacy slot `#plugins`). Simplest; good
   for a single control tied to the bot.
2. **`panel_html`** — a **top-level card** of your own, rendered in the plugin-panels area at the end
   of the page. Use for anything substantial. May include `<style>`; for behaviour use `ui_script`
   (HTML injected via `innerHTML` does **not** execute inline `<script>`).

   Emit the card's **contents**, starting with the `<h2>` — the host supplies the wrapping
   `<div class=card>` itself. Do **not** wrap your own: that nests a card inside a card, which double-
   draws the chrome and puts your `<h2>` where the collapse rule (`.card.col>:not(h2)`) hides it along
   with the body, so the panel can't be reopened.

   ```c
   snprintf(buf, cap, "<h2>My plugin</h2><div class=hint>…</div>", …);   /* no <div class=card> */
   ```
3. **`sections_html`** — add **sections into existing panels**. Emit one or more fragments each tagged
   with a target slot; the page routes each into the card slot of that name:
   ```c
   snprintf(buf, cap, "<div data-slot=\"headset\">…</div><div data-slot=\"source\">…</div>");
   ```
   Built-in slots: `account`, `bot`, `store` (more added over time; a fragment for an unknown slot is
   appended to the plugin-panels area). The slot receives your fragment's inner HTML.
4. **`ui_script`** — a URL to a JS asset (serve it from your `http` hook) that the host loads **once**
   as a real `<script>`, so it actually runs. This is how you **add, modify, or remove** any
   section/panel/element — you get full DOM access plus a small API:

   ```js
   // window.bsdrUI
   bsdrUI.onStatus(function(s){ /* runs every poll with the status object */ });
   bsdrUI.slot('headset');      // the slot element, to append/replace/clear
   bsdrUI.api('/api/plugin/my-plugin/do', {x:1});   // same helper as the panel
   bsdrUI.badge('mycard', on, 'fast');   // the card's header badge (see below)
   ```

   Append your card to `#pluginpanels` — the container the host reserves for script-created cards and
   never rewrites. (`#pluginmanaged`, next to it, holds `panel_html` + config cards and IS rewritten
   every poll; a card appended there would be wiped ~1×/s.) Guard on your card's id so the script is
   idempotent:

   ```js
   (function(){ if (document.getElementById('mycard')) return;
     var c = document.createElement('div'); c.className = 'card'; c.id = 'mycard';
     c.innerHTML = '<h2>My plugin</h2>' + …;          // title only — the host adds the badge + chevron
     (document.getElementById('pluginpanels')||document.body).appendChild(c);
   })();
   ```

### Card headers

Give `<h2>` the **plain title**: no "(my-plugin plugin)" suffix — the card is already grouped under
the plugin panels, so the suffix is just noise. The host turns every card into a collapsible panel
(click-to-toggle, state persisted in `localStorage`, keyed off the header text) and adds a **badge**
in the header that stays visible while collapsed. Report your state into it:

```js
bsdrUI.onStatus(function(s){
  var m = s.myplugin.mode;                                  // e.g. 0=off 1=fast 2=AI
  bsdrUI.badge('mycard', m > 0, m===2?'AI':(m===1?'fast':'off'));   // on -> green pill
  bsdrUI.badge('mycard', null, '3 items');                  // on===null -> neutral, no colouring
});
```

`badge(card, on, text)` takes the card element or its id. Available since the host release that
introduced it; if you target older hosts, feature-detect with `if (bsdrUI.badge) …`.

### API-key fields

A `type=password` box makes the browser look for the login it belongs to, pick the nearest text input
before it as the "username", and autofill that with the user's saved Bigscreen e-mail — silently
overwriting whatever they typed (a model id, an endpoint, a voice). If your card has a token field,
mark it and its neighbours:

```html
<input id=myendp name=my-plugin-endpoint autocomplete=off        placeholder="https://…">
<input id=mymodel name=my-plugin-model    autocomplete=off        placeholder="gpt-4o-mini">
<input id=mytok   name=my-plugin-token    autocomplete=new-password type=password placeholder=token>
```

`new-password` is what stops it: it says the field isn't a login password, so no username is hunted
for. `autocomplete=off` alone is only advisory — browsers ignore it on fields they believe are
credentials — so give each field a distinct `name` too, one that matches no e-mail/username
heuristic.

## 7. Configuration variables

Declare typed config the host renders as a form (in your settings card), persists, and lets you read
back live — no UI code required:

```c
static const bsdr_plugin_cfgvar MY_VARS[] = {
    { .key="api_url", .label="API URL", .type="text",     .def="",  .help="endpoint to call" },
    { .key="token",   .label="Token",   .type="password", .def="" },
    { .key="loud",    .label="Verbose", .type="bool",     .def="0" },
};
/* in the descriptor: .config = MY_VARS, .config_count = 3 */

/* read a value live, anywhere: */
char url[512]; host->config_get("my-plugin", "api_url", url, sizeof url);
```

`type` is `text` (default), `password`, `number`, or `bool`. The host only accepts writes to keys you
declared, so the config endpoint can't be used to write arbitrary namespaced state.

## 9. Talking to another plugin

The tool registry composes plugins for the *model*, but it only carries JSON — no way to hand over PCM —
and plugins are `dlopen`'d `RTLD_LOCAL`, so one can never call another's symbols. ABI 5 adds a broker: a
plugin publishes a named interface, and the plugins that **depend** on it look it up.

```c
/* publisher, from init() */
s->hearing.self = s; s->hearing.subscribe = my_subscribe; s->hearing.unsubscribe = my_unsubscribe;
host->service_publish(BSDR_SERVICE_HEARING, &s->hearing);

/* dependent, from init() — its dep declaration is what guarantees the publisher ran first */
bsdr_hearing_service *ear = host->service_get(BSDR_SERVICE_HEARING);
if (ear && ear->subscribe) ear->subscribe(ear->self, my_cb, s);   /* always null-check */
```

The host brokers the pointer and never interprets it: each name is a contract between those plugins.
Publish from `init()`. The host clears the table on unload (every pointer aims into a plugin about to be
`dlclose`d), so never cache one across an unload, and **unsubscribe in your `shutdown()`** or the
publisher will call into unmapped code.

### `fullbot.hearing`

The bot's hearing is **single-owner**: `host->utterance_subscribe` REPLACES the callback, so a feature
plugin that subscribes to the host silently steals the room from the bot. Ask fullbot instead — it owns
the ear and re-publishes it:

```c
void on_hearing(uint32_t ssrc, const int16_t *pcm, int frames, int channels,
                const char *transcript, void *user);
```

`transcript` is the STT fullbot **already** ran, so a listener never pays to transcribe the same audio
twice; `pcm` is that utterance, for anything acoustic (levels, voice models). Both are owned by the
caller and valid only for the call. You get every utterance, before the wake-word gate — moderation has
to judge what was said to the *room*, not only what was addressed to the bot. It runs on the bot's audio
worker, so a slow listener delays the bot's own reply: do not block.

### `fullbot.audio`

The host lets exactly **one** owner set the per-speaker gain policy, and fullbot takes it while its mode
is active — so a feature plugin must not call `host->audio_gain_policy` itself, or it evicts the bot's
own levels. Ask the bot to steer instead:

```c
bsdr_room_audio_service *au = host->service_get(BSDR_SERVICE_ROOM_AUDIO);
/* ABI 6: focus carries a PRIORITY so several features can steer at once without clobbering. */
if (au && au->focus_at) au->focus_at(au->self, ssrc, BSDR_FOCUS_MICCHECK);  /* bring this speaker up */
if (au && au->focus_at) au->focus_at(au->self, 0,    BSDR_FOCUS_MICCHECK);  /* done — restore the base */
```

Focus is what an age/mic check needs: the person under check comes up **even though their level would
normally mute them** (checking a stranger is the whole point), and the room ducks so it isn't talking
over them. The **owner is never ducked** — the operator must always be able to talk over a check in
their own room.

The **base** is fullbot's per-level policy — the "most open" gain each level grants. On top of it,
features request a temporary focus, and because several may steer at once, a focus carries a
**priority**; the highest active one wins and clearing it falls back to the next, or to the base. The
tiers, low→high: `BSDR_FOCUS_MICCHECK` < `BSDR_FOCUS_FRIEND` < `BSDR_FOCUS_HOST` < `BSDR_FOCUS_OWNER`.
Each caller sets and clears **only its own tier**. `au->focus(ssrc)` is shorthand for the mic-check
tier (the original single-tier call, still present); prefer `focus_at` on an ABI-6+ host and fall back
to `focus` when it's NULL.

## 8. Dependencies on other plugins

A plugin can declare that it needs other plugins to work. List their `name` ids in `deps` (with
`dep_count`):

```c
static const char *const MY_DEPS[] = { "legacy-mic", "some-codec" };
/* in the descriptor: .deps = MY_DEPS, .dep_count = 2 */
```

The requirement is enforced in **two places**:

- **At load time** (the agent) — a plugin is init'd only after every plugin it names has itself loaded
  successfully, and it is loaded **before** it, so your `init` may rely on a dependency already being
  up. If a dependency is missing, disabled, ABI-incompatible, fails its own `init`, or the graph has a
  **cycle**, your plugin is skipped with a warning and never runs. Load order is resolved automatically;
  shutdown is reverse order, so a dependent tears down before what it depends on. Names are descriptor
  `name` ids, not file names (usually identical).
- **At install time** (the store) — installing a plugin first fetches and installs any missing
  dependency, recursively, so a plugin *can't be installed without the ones it needs*. Already-present
  dependencies (installed, or shipped with the app) are left as-is.

Keep the descriptor's `deps` in sync with **`DEPENDS`** in `plugins/<name>/store.conf` (space/comma
separated slugs) — the descriptor drives loading, `DEPENDS` drives the store's install-time resolution.
An admin can also edit a version's dependencies from the store's plugin page. Dependencies are recorded
per **version**, so they can change as a plugin evolves.

## 9. Versioning, packaging & the store

- Your plugin carries its **own version** (`plugins/<name>/VERSION`, independent of bsdrX). Keep the
  descriptor's `.version` in sync. Ship plugin updates without releasing bsdrX.
- The build stamps N (`BSDR_PLUGIN_ABI` it compiled against) and X (`ABI_MAX` from `store.conf`, default
  `0`) into the zip name (`bsdrX-plugin-<name>-<platform>-<arch>-<version>-abi<N>.zip`) and the store.
- The store serves the agent **the newest non-obsolete version whose `[N..X]` contains the agent's
  current ABI**, for its platform/arch. An admin can additionally **mark a specific version obsolete**
  to withdraw it regardless of ABI. The agent's in-app Plugin Store shows an **update** when a newer
  compatible version than the installed one exists.
- Build one plugin locally: `./distribute.sh --plugin <name>` (add `--push-plugins` to upload).

## 10. Other hooks

- `mic_keepalive_period_ms(state)` (ABI 1) — return a period in ms > 0 to override the bot-mic
  keepalive cadence, or 0 to leave the host default. The first plugin returning > 0 wins.

### Bot host-service surface (ABI 4, in progress)

The host is growing a set of services that let a **bot** live entirely in plugins (see
`PLAN-bot-plugin.md`). The first, load-bearing piece is the **tool registry**: plugins register named
tools that a bot plugin exposes to its LLM loop, so several plugins compose one agent without any
plugin→plugin ABI (everything is mediated by the host).

```c
/* register a tool (typically in init); group is a required-permission bitmask */
host->tool_register("kick", "Remove a user from the room",
                    "{\"type\":\"object\",\"properties\":{\"user\":{\"type\":\"string\"}}}",
                    MY_MODERATOR_BIT, my_kick_fn, state);

/* a bot plugin builds a caller's toolset and dispatches calls back to the owner: */
char tools[8192]; host->tool_list_json(caller_mask, tools, sizeof tools);
char result[1024]; host->tool_invoke("kick", args_json, caller_level, caller_mask, result, sizeof result);
```

`tool_invoke` re-checks the caller's mask against the tool's group before dispatching (defence in
depth), and the host drops a plugin's tools automatically when it unloads.

Other ABI-4 services already available: **identity** (`roster_json`, `owner_ssrc`, `resolve_speaker`),
**moderation** (`kick_user`, `ban_user`, `reset_room`), **speech** (`speak`, `stt`), **avatar reads**
(`avatar_state`, `local_legacy_user_id`), **desktop** (`screenshot_jpeg`), **LLM** (`llm_complete` — one
round-trip for a plugin-owned agentic loop), **JSON** (`json_escape`), and **room hearing**
(`utterance_subscribe` — the host runs VAD and delivers whole utterances to your callback on a worker
thread, bypassing the in-core bot), **presence modes** (`bot_mode_register`/`bot_mode_get` — register a
selectable bot mode, e.g. "fullbot", that the host activates via your `on_active(1/0)` callback when the
operator selects/leaves it; the core always offers the bare built-in "audio"), and **avatar/presence
control** (`avatar_enable` to raise/drop the avatar actuator, `avatar_set_follow`/`avatar_follow` for
follow-me), and **per-speaker audio** (`audio_gain_policy` — register a `bsdr_gain_policy_fn` and the
core applies your per-SSRC gains to the room mix; none => unity). Still to come (per
`PLAN-bot-plugin.md` §5): input injection, and (if a plugin should own avatar pose) a raw
avatar-channel send.

## 11. ABI history

| ABI | added |
|-----|-------|
| 1   | `init`/`shutdown`/`http`/`ui_html`/`mic_keepalive_period_ms`; host `log`/`http_respond`/`json_get_double`/`config_dir`. |
| 2   | plugin `abi_max`, `panel_html`, `sections_html`, `ui_script`, `config`/`config_count`; host `json_get_str`/`config_get`/`config_set`. |
| 3   | plugin `deps`/`dep_count` — dependency-ordered loading + store install-time dependency resolution (§8). |
| 4   | host bot-service surface (in progress): tool registry `tool_register`/`tool_unregister`/`tool_list_json`/`tool_invoke` + `bsdr_tool_fn`. |
| 5   | plugin-to-plugin services: host `service_publish`/`service_get`; `bsdr_hearing_service` + `BSDR_SERVICE_HEARING`, `bsdr_room_audio_service` + `BSDR_SERVICE_ROOM_AUDIO` (§9). |
| 6   | priority-arbitrated audio focus: `bsdr_room_audio_service.focus_at(ssrc, priority)` + `BSDR_FOCUS_MICCHECK/FRIEND/HOST/OWNER` tiers, so features steer the mix without clobbering (§9). |
