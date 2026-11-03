# Architecture and protocol

Three parts, one responsibility each:

```
Claude Code session A ─┐ (mod: tessera-bridge, one instance per session)
Claude Code session B ─┤ POST /api/tessera_claude/v1/{report,claim,end}   long-lived token
Claude Code session C ─┘            │
                                    ▼
                   Home Assistant: custom_components/tessera_claude
                   registry (sessions, questions, limits, single winner)
                   sensor.claude_sessions …, services get_overview / get_question / get_hint / answer
                                    │  entity attributes (Tessera app → tile)      ▲ tessera::send (call_service, return_response)
                                    ▼                                               │
                   Tessera screen: plugin claude_sessions (tile, cards, popup, top bar item)
```

- **The mod** sees exactly one session: the engine runs one session per process (`$.session.id()`; after `/clear`
  or a resume it goes on under another id), and its hooks run in every session that loads the plugin, whether that
  session is selected in the Desktop app or not. There is no API to see other sessions from a mod, so every session
  reports itself and nothing else. Nothing in the mod is shared with another session: the state is keyed by session
  id, and a new id starts a new bridge.
- **The integration** is the registry and the one place that decides who answered a question. It is in-memory: a
  restart of Home Assistant forgets everything, and the bridges' next reports (within 1.5 s while a question is
  open, 15 s otherwise) rebuild it. Handles the display uses carry a token of the registry's life, so an answer from
  before a restart fits nothing after it.
- **The plugin** draws and answers. It talks to Home Assistant only: the sensor's attributes come with the tile, and
  the services through `tessera::send` (the Tessera app calls them with `return_response` and hands back the answer,
  bounded: texts 48 bytes, about 3 KB in all).

## Identity

| Thing | Id | Who makes it |
|---|---|---|
| Installation of the mod (a computer) | `instance`: 8 random characters, kept in the mod's `$.store` | the mod, once |
| Session | `<instance>.<session id>` | the mod (`$.session.id()`) |
| Question | the AskUserQuestion call's `tool_use_id`, plus a `nonce` per registration | the mod |
| Part of a question, option | `p0`…`p3`, `o0`…`o3` | the mod |
| Handle of a session, of a question | `s<token><n>`, `q<token><n>` (`sab3`, `qab7`): short, never reused | the integration |

Texts (titles, labels, question texts, session names) are for showing only; nothing is matched by them.

## The mod → the integration

All JSON, `POST`, `Authorization: Bearer <long-lived token>`.

### `/api/tessera_claude/v1/report`

The whole state of one session, sent on every change, every 15 s as a heartbeat, and every 1.5 s while a question of
the session is open (the reply doubles as the poll):

```json
{
  "instance": "k3j9x2ab", "session": "7c0e…", "seq": 42,
  "name": "tessera-bridge", "instance_name": "workstation", "model": "claude", "cwd": "/home/me/tessera-bridge",
  "state": "asking",                      // idle | working | asking | permission
  "detail": "",                           // the permission hint's text while state is permission
  "context": { "percent": 37, "tokens": 370000, "window": 1000000 },   // or null while unknown
  "limits": [ { "kind": "five_hour", "percent": 44, "resets_at": "2026-11-03T22:00:00Z" } ],
  "questions": [ { "id": "toolu_01…", "nonce": "x8z1k0p3", "parts": [
      { "id": "p0", "header": "Approach", "text": "Which way?", "multi": false,
        "options": [ { "id": "o0", "label": "A", "description": "…" }, { "id": "o1", "label": "B", "description": "" } ] } ] } ]
}
```

Reply: `{ "ok": true, "handle": "sab3", "questions": { "toolu_01…": { "status": "open|answered|cancelled|unknown",
"by": "display|pc", "answer": { "p0": ["o1"] }, "nonce": "…", "handle": "qab7" } } }`. A report with a `seq` lower than
the last one seen changes nothing (`"stale": true`) but is still answered as a poll. A question the session no longer
lists is cancelled; one listed with another nonce is a new registration (new handle), the old one over.

### `/api/tessera_claude/v1/claim`

The PC answered or dismissed the question: `{ "instance", "session", "id", "nonce", "by": "pc", "outcome":
"answered|cancelled" }`. Reply `{ "accepted": true }` when the PC's answer stands, or `{ "accepted": false, "status":
"answered", "by": "display", "answer": {…} }` when a display was first: then the mod returns the display's answer to
the model, so exactly one answer takes effect. Unknown question (a restart in between): accepted.

### `/api/tessera_claude/v1/end`

`{ "instance", "session", "reason" }`: the session shows as ended for a minute, its questions are cancelled, then
it goes.

## The display → the integration

Services with a response; the plugin names them under `permissions.ha_commands`.

| Service | Data | Response |
|---|---|---|
| `tessera_claude.get_overview` | – | `{ "sessions": [ { "h", "name", "pc", "state": "idle|work|ask|perm|lost|end", "ctx", "asks", "q", "detail" } ], "limits": { "at", "kinds": { "five_hour": { "percent", "resets_at" }, … } } }` |
| `tessera_claude.get_question` | `{ "question": "qab7" }` | `{ "h", "rev", "status", "session", "name", "parts": [ { "id", "header", "text", "lines": ["…≤46 bytes"], "multi", "options": [ { "id", "label", "description" } ] } ] }` |
| `tessera_claude.get_hint` | `{ "session": "sab5" }` | `{ "h", "name", "state", "detail" }` |
| `tessera_claude.answer` | `{ "question": "qab7", "rev": 41, "answers": { "p0": ["o1"], "p1": ["o0", "o2"] }, "display": "kitchen" }` | `{ "accepted": true }` or `{ "accepted": false, "reason": "answered|stale|cancelled|ended|unknown|bad_answer", "by": "display|pc" }` |

An answer is checked before it counts: every part answered, one option for a single choice, only option ids that
exist, the `rev` the display saw, the question still open and its session alive. The first valid answer wins; every
later one (a second display, a double tap, the PC) is refused with the reason.

## Sensors

| Entity | State | Attributes |
|---|---|---|
| `sensor.claude_sessions` | sessions alive | `rev`, `event` (`question:qab7` / `hint:sab5`, changes with every new one), `event_rev`, `open`, and parallel lists of at most 12 sessions, asking ones first: `h`, `names` (≤24 chars), `states`, `ctx` (-1 unknown), `asks`; the limits `l5h`, `l5h_at` (epoch), `lwk`, `lwk_at`, `lx_names`, `lx` |
| `sensor.claude_questions` | open questions | `questions` (handle, session, text), `event`, `event_rev` |
| `sensor.claude_limit_5h`, `sensor.claude_limit_week` | percent used, `unknown` until reported | `resets_at`, `resets_in` |
| `sensor.claude_5h_reset` | `1h 09m` | `resets_at` |
| `sensor.claude_limits` | other windows | one attribute per kind (a model's weekly window) |

The limits are the account's, from the computer that reported last; never summed over sessions. A session's context
is its own, in the lists; there is no single `sensor.claude_context` any more (it could only ever show one session).

## Events on the bus

`tessera_claude_question` (`handle`, `session`, `text`) once per new question and `tessera_claude_hint` (`handle`,
`session`, `detail`) once per new permission hint, for automations of your own (a notification, a light). With a
player in the integration's options the same moments play a chime and the spoken text through Chime TTS.

## States of a session

| State | From |
|---|---|
| `idle` | between turns |
| `working` | `turn.start` until `turn.complete` |
| `asking` | an AskUserQuestion call is open |
| `permission` | a permission ask stayed open longer than the hint delay (default 8 s); `detail` says which call |
| `lost` | no report for 45 s (the PC sleeps, the network is away); forgotten after 10 min |
| `ended` | `session.end`; gone after a minute |

A session's `turn.complete`, abort or end settles only that session's questions and hints.

## Limits of the transport, and what is done about them

- A text in a service's answer is cut to 48 bytes by the Tessera app: a question travels as `lines` of ≤ 46 bytes
  (at most 6, the last ending in …), option labels and descriptions are bounded at the integration (descriptions
  40 chars), session names at 24.
- A tile's data is about 2.6 KB: the sensor's lists carry at most 12 sessions; the overview service (no such
  limit, about 3 KB) carries them all and the cards page through them.
- An answer the display sends is at most 512 bytes: handles and option ids only, never texts.
- Free text ("Other") cannot be typed on the screen: **On the PC** leaves the question to the dialog there, which
  stays open the whole time anyway.
- A permission prompt is never answered on the screen; the mod only reports it.
