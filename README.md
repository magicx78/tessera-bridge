# tessera-bridge

Every running Claude Code session on the Tessera touch screens in Home Assistant, and Claude's questions answered
there. Three parts in this repository, one responsibility each (docs/PROTOCOL.md has the whole protocol):

| Part | Where | What it does |
|---|---|---|
| **The mod** `tessera-bridge` | `hooks/`, `.claude-plugin/` | A Claude Code mod (Desktop app's Code tab or terminal). Every session that loads it reports itself to Home Assistant under its own id: its state (idle, working, asking, waiting for a permission), its context fill, the account's rate limits, and the questions Claude asks (AskUserQuestion), with ids for every part and option. When a screen answers, the mod hands the answer to the model; when the PC answers, it tells Home Assistant. |
| **The integration** `tessera_claude` | `custom_components/tessera_claude/` | The registry in Home Assistant: sessions, questions, limits. It settles who answered a question first (the PC or a screen, two screens, a double tap), gives the screens sensors and services with responses, fires events for automations, and plays a chime and the spoken question through Chime TTS when a player is set. |
| **The display plugin** `claude_sessions` | `tessera-plugin/claude_sessions/` | A regular Tessera plugin: a tile with the sessions and their states, a sessions card with pages, a question card that answers on the screen (several parts, multi-select, On the PC), a popup for a new question or a waiting permission, a top bar item with the open questions. |

Several sessions may ask at once, from several computers; identical titles and options never collide, because
nothing is matched by its text. A session in the background of the Desktop app is served like the selected one.

## Set up

### 1. Home Assistant: the integration

HACS → Integrations → custom repository `https://github.com/magicx78/tessera-bridge` (category Integration), or
copy `custom_components/tessera_claude` into `/config/custom_components/`. Restart Home Assistant, then Settings →
Devices & services → Add integration → **Tessera Claude**. Everything in its form is optional: a `media_player` for a
chime and the spoken question (the Chime TTS integration must be installed for that), the chime preset, a `tts`
entity, the language, whether the question is read aloud, whether a permission prompt is announced. Change them
later under the integration's Options.

Then, in your Home Assistant profile under Security, create a long-lived access token for the mod.

### 2. Claude Code: the mod

In a Claude Code session (Desktop app, Code tab, or the terminal, v2.1.287 or later):

```
/plugin marketplace add magicx78/tessera-bridge
/plugin install tessera-bridge@tessera-bridge
```

Then set its options in the plugin's configuration (`/plugin`, Installed, tessera-bridge): the Home Assistant URL as
this computer reaches it, the token, optionally a name for this computer (shown in the session list), and the
permission hint settings. Run `/reload-plugins` in open sessions, or start new ones. Without a token the mod does
nothing.

To load a checkout for one session instead: `claude --plugin-dir /path/to/tessera-bridge`.

### 3. Tessera: the plugin

Plugins → **Add with a link** → `https://github.com/magicx78/tessera-bridge/tree/main/tessera-plugin/claude_sessions`
(or copy `tessera-plugin/claude_sessions` into `/config/tessera-plugins/claude_sessions/` as a test folder). Add it
to a screen; when asked for the sessions sensor, choose `sensor.claude_sessions`. Place the **Claude sessions** tile
in Layout. `tessera-plugin/claude_sessions/README.md` has the rest.

## How a question travels

1. Claude calls AskUserQuestion in session A. The mod registers the question with Home Assistant (ids for the call,
   its parts and options), keeps the dialog in the app open beneath, and polls Home Assistant every 1.5 s.
2. Home Assistant raises `event` on `sensor.claude_sessions`; the screen shows a popup (a tap opens the question) and
   a badge on the tile; Chime TTS says it when a player is set.
3. A tap on an option sends `tessera_claude.answer` with the question's handle, its revision and the option ids. The
   integration checks it and answers `accepted` once, to exactly one answer.
4. The mod's next poll sees the answer, hands it to the model, and the app's dialog is over. If the PC answered first,
   the mod claims that at the integration; if a screen was quicker there, the screen's answer is the one the model
   gets.

## Migration from version 0.1

- The `input_text.claude_screen_answer` helper, the `esphome.screen_alert` automation and the alerts on the screens
  are no longer used: the plugin answers through the integration's services. Delete the helper and the automation
  when you like; nothing writes to them any more.
- `sensor.claude_limit_5h`, `sensor.claude_limit_week` and `sensor.claude_5h_reset` keep their ids and meaning; they
  are the integration's entities now, so remove the old REST-created states once (they linger until a restart).
- `sensor.claude_context` is gone: it could only ever hold one session's value. Every session's context is in the
  lists of `sensor.claude_sessions` (`names`, `ctx`), on the tile and in the sessions card.
- The sound options moved from the mod to the integration (they belong to Home Assistant, not to a session). The
  mod's options `screen`, `answerEntity`, `soundPlayer`, `soundChime`, `soundTts`, `soundLanguage` and `soundSpeak`
  are not read any more; `haUrl`, `haToken`, `permissionHints` and `hintDelay` stay.
- Dashboards that used the old sensors keep working; the meter tiles on a screen can show the new ones too, but the
  plugin's tile shows all of it in one place.

## Development

- Mod: `claude plugin validate .` and `claude plugin test .` (the tests in `tests/tessera-bridge.test.ts` run with
  the engine's own test kit; no session, no network). The types the hooks are written against are written by
  `/plugin-types` into `.claude-plugin/types/` (not checked in).
- Integration: `python3 -m pytest tests/test_registry.py` for the registry's rules (plain Python), and with
  `pytest-homeassistant-custom-component` installed, `python3 -m pytest tests/test_integration.py` against a real
  Home Assistant core.
- Plugin: from a checkout of `MaxGramser/tessera-plugins`, `python3 tools/check.py <this repo>/tessera-plugin/claude_sessions`,
  `python3 tools/preview.py <…> --strict`, `python3 tools/store_image.py <…>`.
