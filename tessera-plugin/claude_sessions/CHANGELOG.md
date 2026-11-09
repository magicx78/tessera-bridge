# Changelog

## 0.1.12
- The bar under a background task now FILLS as the task goes: at "Step 1 of 3" it is one third full (before it showed what was left, two thirds). Without steps it fills with the task's age over eight hours (before it shrank). Colours (accent, amber from one hour, red from four) are unchanged; the tile and the card "Claude in the background" both follow. Nothing to do.
- The animation of the activity tile uses at most two lanes on every tile size (the muncher had up to six on a tall tile), so a 2x3 tile looks calmer. Nothing to do.

## 0.1.11
- The activity tile can be 2x3 (and 1x3), and its animation grows with the tile: on a wide tile the picture fills the whole right half from under the head to the bottom edge. The mosaic lays as many square stones as the room holds (more rows on a tall tile), the muncher gets a lane for each stretch of height, and the spark takes the height (two sparks on a tall tile). 1x1, 2x1 and narrow tiles look as before. Nothing to do; place the tile as 2x3 in the layout editor if you want it taller.
- The card "Claude in the background" shows how many actions (tool calls) an agent has made so far, "5 actions", when it reports neither steps nor a note, instead of "no progress reported". Shells, and agents with steps or a note, look as before. Needs the integration 0.2.6 (the sensor's `bg_calls`); with an older integration the card looks as before.

## 0.1.10
- The two popup switches ("Popup on a question", "Popup on a permission hint") take effect at once, whether switched on the glass or in Home Assistant: off closes a popup of that kind that is open and drops a request still on its way, so no popup opens a moment later; on shows what waits right now (a question only while one is open, even after a later hint). Nothing to do.
- A new settings row "Announce" ("Ansage"): Speech, Tone or Nothing, for a new question or permission hint. The choice is kept by the integration and shows at once on every screen. Needs the integration 0.2.5 (service `set_notify`, the sensor's `notify` attribute); with an older integration the row shows "Speech" and a choice does nothing.
- The limits tile has an option "Window" in the editor: "Both" (default, as before), "5 hours" or "Week". With one window the tile looks like a meter tile: the percent big, the window's name under it, the bar along the bottom and "reset in ..." in the top corner where there is room (else under the bar). Pick it per tile, e.g. two 1x1 tiles side by side.
- The background tile draws a bar under each task, coloured by how long it runs (the accent in the first hour, amber from one hour, red from four). The bar shows what is left: by the task's steps when it reports them ("2/5" at the right), else it shrinks over eight hours. A tap opens a new card "Claude in the background": every task with its session, since when, its bar, "Step 2 of 5" and the note it gave last ("no progress reported" where it gives none). Real steps need the integration 0.2.5 (`bg_done`/`bg_total`, service `get_background`) and the tessera-bridge mod 0.2.4; with older ones the bars go by age and the card shows the sensor's rows without notes.
- The activity tile's animation takes the whole right half of a wide tile, down to the bottom; the number, the words and (on 2x2) the session names keep the left half.
- Re-add nothing: the tiles keep their places. Update the integration to 0.2.5 and the mod to 0.2.4 for the Announce row and real progress.

## 0.1.9
- A second permission hint of the same session opens its popup again: the sensor's `event` keeps its text for the same session (`hint:<handle>`), so the screen now follows the event counter (`event_rev`) too, and a counter that moves without a new text is a new event. The two values of one event arriving a moment apart are still one event.

## 0.1.8
- The activity tile moves: a small picture that works while Claude works, sleeps at rest, waits with an approval (amber) and points at a question (on the red ground). One step a second, with the tile's clock. Each tile picks its own in the editor, option "Animation": "Building a mosaic" (default: a 5x3 mosaic laid stone by stone), "Mosaic muncher" (eats a row of stones), "Claude spark" (a wave runs round eight arms) or "None". Beside the words on a wide tile, above the number on 1x1; hidden where the tile has no room for it. Blocks and labels only, so every screen draws it.
- A fourth tile, `limits` ("Claude limits", 1x1 to 3x2): the account's windows as bars, five hours and the week, each with its percent (amber from 70 %, red from 90 %), a mark where an even pace through the window would stand (a fill past the mark spends ahead) and, where there is room, when it starts anew. Any window of one model the sessions report (`seven_day_<model>`, the sensor's `lx_names`/`lx`) comes as a further bar by itself, so a Fable or Opus week shows the day Claude Code reports one; today it reports only five hours and the week. A tap opens the sessions card.

## 0.1.7
- A third tile, `background` ("Claude in the background", 1x1 to 3x3): what runs in the background of the sessions (shells started with `run_in_background`, background agents, monitors, workflows), newest first, each with its kind, its title and when it started ("14:05", on two columns or more also "· 12 min"); a badge with the number. On 1x1 the number big and "since" the oldest. Needs the integration 0.2.4 (`sensor.claude_background`) and the mod 0.2.2, which reports each session's background tasks.

## 0.1.6
- A second tile, `activity` ("Claude active", 1x1 to 2x2): the number of sessions working, asking or waiting for approval, since when the oldest of them has been at it ("for 12 min") or, at rest, when the last one was ("last 1 h ago"), "of N sessions", and a badge with the open questions. The tile turns red while a question waits and amber while an approval waits at the PC; a tap opens the first open question, else the sessions card; on 2x2 it lists the active sessions. Needs the integration 0.2.3 (the sensor's new attributes `q`, `active`, `active_since`, `last_active`).
- The overview tile takes `q` too, so both tiles on one screen fill the same data.
- While a question waits, the whole activity tile turns red, its words white, so it is seen from across the room.
- A question answered on another screen or at the PC now closes its popup and its question card on every screen, after a moment.

## 0.1.5
- A single choice is marked by a tap and sent with the Send key (Next for a part before the last), as a multi-select already was: on the first hardware tests one tap answered the model before the person had read the question. The chosen option is lit in the accent on every part.

## 0.1.4
- A question or hint request the app did not answer is sent again every 10 s, up to six times: after a start the first event was handled while the screen still waited for the app, the request could not be sent, and nothing asked again, so the open question got no popup (seen on hardware after a reboot).

## 0.1.3
- A question asked while the screen was off, rebooting or away from Home Assistant gets its popup once the screen is back: the first `event` value after a start was treated as history and skipped, so a question open at that moment never showed (seen on hardware after a Home Assistant restart). The registry's reply still decides: a popup only for a question that is open, a hint only while it waits.

## 0.1.2
- The tile shows the context fill of every session, idle ones too (before only a working one's). Needs the tessera-bridge mod 0.2.1 and the integration 0.2.1 for steady limits: the account's limits come from the session that measured them last, not from the idle session whose heartbeat arrived last, so the limits line no longer flips between old readings and "–".

## 0.1.1
- The open question card asks for its question again at most every 1.5 s: the registry's `rev` moves with every report of every session, and a request per change sent `get_question` several times a second while sessions were polling (seen on the first hardware test).
- Debug log lines (tag `claude_sessions`): tile created, tile data taken, every reply from Home Assistant, every answer sent, so a screen's log shows the plugin at work.
- Limits line: the reset time is formatted without printf warnings on the ESP32-P4 toolchain (uint32_t is unsigned long there). First build on real hardware (Waveshare ESP32-P4 DEV-KIT, 7" 800x480, ESPHome 2026.10.2).

## 0.1.0
- First version: a tile with every Claude session and its state, a sessions card with pages, a question card that answers Claude's questions on the screen (several parts, multi-select, On the PC), a popup for a new question or a waiting permission, a top bar item with the open questions, and the account's limits. Needs the Tessera Claude integration and the tessera-bridge mod.
