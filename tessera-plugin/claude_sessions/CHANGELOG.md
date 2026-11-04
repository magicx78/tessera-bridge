# Changelog

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
