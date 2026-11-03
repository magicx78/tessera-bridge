# Changelog

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
