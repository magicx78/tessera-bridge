# tessera-bridge

A mod for the Claude desktop app that connects a session to the Tessera touch screens in Home Assistant (the "Tessera Screen Manager" app, ESP32 panels).

What it does:

- **Questions on the screen.** When Claude asks a question with choices (the AskUserQuestion dialog), the same question appears as an alert on the Tessera screens, with the choices as buttons. A tap on the screen answers the question in the app; answering in the app takes the alert off the screen. Three or four choices are shown one alert at a time ("Weiter ›" goes on, "Am PC" leaves it to the computer).
- **Permission hints.** When the app waits for a permission ("may I run this command?"), the screen shows a hint alert with the tool and what it is about. The permission itself is still answered on the computer.
- **Usage sensors.** The account's rate-limit windows (5-hour, weekly, per model) and the session's context fill are pushed to Home Assistant as sensors (`sensor.claude_limit_5h`, `sensor.claude_limit_week`, `sensor.claude_context`, `sensor.claude_5h_reset`, ...), so they can be shown as meter tiles on a screen or on any dashboard.

- **Sound.** With a `media_player` set as the sound player (the screen's own speaker, for example), each alert is announced through the Chime TTS integration: a short chime, then the question read aloud. Both parts can be turned off.

## Setup

1. In Home Assistant, create an `input_text` helper named `claude_screen_answer` (the screen's buttons write the answer there).
2. In your Home Assistant profile, under Security, create a long-lived access token.
3. In the app's config menu set the mod's options: the Home Assistant URL as this PC reaches it, the token, and optionally one screen's device name (empty reaches every paired screen). For sound, set the sound player to a `media_player` entity, install the Chime TTS integration (HACS), and name the speech engine (a `tts.*` entity such as `tts.home_assistant_cloud` or `tts.piper`) and the language.
4. Add the usage sensors to a page of a screen as tiles (`display: meter`), for example through the `esp_screens_add_tile` event.
5. Optional backup for the answer channel, in case a screen's firmware does not run the alert's button action: an automation on the event `esphome.screen_alert` that writes `{{ trigger.event.data.action }}|{{ trigger.event.data.title }}` to the helper when `action` is `ok` or `button2`. The mod understands both formats.

Without a token the mod does nothing. Permission hints appear only when an ask stays open for longer than the hint delay (8 seconds by default), because the app's auto mode reports every call as an ask first; raise the delay there if hints still show up for calls the app approves by itself.
