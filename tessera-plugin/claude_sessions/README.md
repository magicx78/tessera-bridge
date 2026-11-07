# Claude sessions

> Needs the **Tessera Claude** integration in Home Assistant and the **tessera-bridge** mod in Claude Code (Desktop app or terminal). Both are in the same repository as this plugin.

Every running Claude Code session on the screen: which ones work, wait for an answer or for an approval, how full their context is, and the account's 5-hour and weekly limits. When Claude asks a question with choices, the screen shows it and a tap answers it — in the right session, also one that is not selected on the PC. Several sessions may ask at once; each question stays until it is answered, on the screen or on the PC.

## What it adds

- **Claude sessions** (tile, 1x1 to 3x3): the sessions and their states, the open questions as a badge, the limits on the last line (from 2 rows up). A tap opens the sessions card.
- **Claude active** (tile, 1x1 to 2x2): whether Claude works, since when and in how many sessions; red as a whole while a question waits, and a tap opens it. Option **Animation**: a small picture that works, sleeps and waits with you — *Building a mosaic*, *Mosaic muncher*, *Claude spark* or *None*. On a wide tile it takes the right half; the number and the session names stay at the left.
- **Claude limits** (tile, 1x1 to 3x2): the 5-hour and the weekly limit as bars, each with a mark where an even pace would stand and when it starts anew; amber from 70 %, red from 90 %. A window of one model that Claude Code reports (a Fable or Opus week, say) comes as a further bar. Option **Window**: *Both* (default), *5 hours* or *Week* — with one window the tile looks like a meter tile: the percent big, the bar at the bottom, "reset in ..." in the corner. Two 1x1 tiles side by side show both windows large.
- **Claude in the background** (tile, 1x1 to 3x3): what runs in the background of the sessions (shells, agents, monitors, workflows), newest first, with its start time and a bar of what is left: by the task's steps where it reports them ("2/5"), else by its age; the accent in the first hour, amber from one hour, red from four. A tap opens the background card. Reads `sensor.claude_background`.
- **The background card**: every background task with its session, since when, its bar, "Step 2 of 5" and the note it gave last.
- **The sessions card**: every session with its computer, context and what it waits for, a page at a time. A tap on a session with a question opens that question.
- **The question card**: the question, its options as keys (several parts one after another; a multi-select with a Send key), and **On the PC** to leave it to the computer. The answer is confirmed by Home Assistant: exactly one answer counts, whoever was first.
- **A popup** when a new question arrives (a tap opens it) and when a session waits for a permission on the PC (which is answered there). Each has a switch under Settings, and the switches take effect at once: off closes an open popup of that kind. The row **Announce** chooses what Home Assistant plays for a new question or hint: *Speech*, *Tone* or *Nothing*.
- **A top bar item** with the number of open questions.

## Set up

1. In Home Assistant, install the Tessera Claude integration (HACS, custom repository `https://github.com/magicx78/tessera-bridge`, or copy `custom_components/tessera_claude` into `/config/custom_components/`), restart, add it under Settings → Integrations. A sound player, chime and speech engine there are optional (Chime TTS).
2. In Claude Code, install the tessera-bridge mod (`/plugin marketplace add magicx78/tessera-bridge`, then `/plugin install tessera-bridge@tessera-bridge`) and set the Home Assistant URL and a long-lived access token in its options. Every session that loads the mod reports itself.
3. Add this plugin to a screen in Tessera: Plugins → **Add with a link** → `https://github.com/magicx78/tessera-bridge/tree/main/tessera-plugin/claude_sessions`. When asked for the sessions sensor, choose `sensor.claude_sessions`. The screen builds once.
4. In Layout, place the tiles you want (Plugins group): **Claude sessions**, **Claude active**, **Claude limits** with `sensor.claude_sessions`, **Claude in the background** with `sensor.claude_background`; optionally the **Claude questions** item in a top bar. The animation of **Claude active** and the window of **Claude limits** are chosen in the tile's settings.
5. Settings → Plugins → Claude sessions: whether a new question or a waiting permission opens a popup, and what is announced (**Announce**: Speech, Tone or Nothing; needs the integration 0.2.5).
6. For real progress on the background bars, the integration 0.2.5 and the mod 0.2.4; without them the bars go by age.

## Good to know

- Free text is not typed on the screen: such a question shows its choices only, and **On the PC** leaves it there.
- A permission prompt ("may I run this command?") is never answered on the screen: the popup is a hint.
- The screen shows "lost" for a session whose bridge stopped reporting (the PC sleeps, the network is away); it goes after ten minutes.
- The account's limits are the account's: the same for every session, never added up. A model window shows only once Claude Code reports one; until then the limits tile shows the 5-hour and the weekly bar.
- The animation moves one step a second with the tile's clock: no timer runs on the screen for it.
- Texts on the screen are bounded by the transport: a question travels as lines of up to 48 bytes, at most six lines; a long option description is cut.

## Privacy

Nothing leaves your home: the screen talks to Home Assistant only, and Home Assistant talks to the Claude Code sessions on your computers. The plugin uses no web service.
