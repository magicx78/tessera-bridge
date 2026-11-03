# Changelog

## 0.1.1
- Debug log lines (tag `claude_sessions`): tile created, tile data taken, every reply from Home Assistant, every answer sent, so a screen's log shows the plugin at work.
- Limits line: the reset time is formatted without printf warnings on the ESP32-P4 toolchain (uint32_t is unsigned long there). First build on real hardware (Waveshare ESP32-P4 DEV-KIT, 7" 800x480, ESPHome 2026.10.2).

## 0.1.0
- First version: a tile with every Claude session and its state, a sessions card with pages, a question card that answers Claude's questions on the screen (several parts, multi-select, On the PC), a popup for a new question or a waiting permission, a top bar item with the open questions, and the account's limits. Needs the Tessera Claude integration and the tessera-bridge mod.
