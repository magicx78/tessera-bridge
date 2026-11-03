"""Constants of the Tessera Claude integration."""

DOMAIN = "tessera_claude"

# The HTTP API the tessera-bridge mod talks to, under Home Assistant's own /api/ (a long-lived token signs in).
API_PATH = "/api/tessera_claude/v1/{action}"

SIGNAL_UPDATE = f"{DOMAIN}_update"

# Options (the config flow and the options flow set them; all optional).
CONF_PLAYER = "player"
CONF_CHIME = "chime"
CONF_TTS = "tts"
CONF_LANGUAGE = "language"
CONF_SPEAK = "speak"
CONF_HINTS = "hints"

DEFAULT_CHIME = "chirp"
DEFAULT_LANGUAGE = "de-DE"

# Services the display plugin (and anyone else) calls, each answering with a response.
SERVICE_OVERVIEW = "get_overview"
SERVICE_QUESTION = "get_question"
SERVICE_HINT = "get_hint"
SERVICE_ANSWER = "answer"

# Events on Home Assistant's bus, for automations (a chime of your own, a notification).
EVENT_QUESTION = f"{DOMAIN}_question"
EVENT_HINT = f"{DOMAIN}_hint"

PRUNE_SECONDS = 10
