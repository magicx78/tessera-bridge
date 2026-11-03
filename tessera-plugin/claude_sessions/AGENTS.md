# claude_sessions: a plugin for Tessera

This folder is one plugin for Tessera, the ESP32 touch screens for Home Assistant, inside the tessera-bridge
repository (the mod and the Home Assistant integration it needs are beside it). Its rules, its docs and its tools
live in the plugins repository: clone it beside this repository, then read its AGENTS.md before you change anything.

```sh
git clone https://github.com/MaxGramser/tessera-plugins ../../../tessera-plugins
```

- The rules and the order to read the docs in: `../../../tessera-plugins/AGENTS.md`.
- Check the plugin, after every change: `python3 ../../../tessera-plugins/tools/check.py .` (must print `claude_sessions: ok`).
- See what it draws on every kind of screen, after every change to its C++, its manifest or `preview.yaml`:
  `python3 ../../../tessera-plugins/tools/preview.py . --strict` (`--setup` once first). The first build of each
  screen takes minutes. Read the findings it prints, `.preview/summary.json` and the pictures in `.preview/`.
- Then make its picture on the Plugins page: `python3 ../../../tessera-plugins/tools/store_image.py .` writes `store.png`.
