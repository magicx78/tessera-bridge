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
CONF_NOTIFY = "notify"
# The old yes/no "read the question aloud"; only the migration and Announcer.reload() still look at it.
CONF_SPEAK = "speak"
CONF_HINTS = "hints"

DEFAULT_CHIME = "chirp"
DEFAULT_LANGUAGE = "de-DE"

# What a new question or hint sounds like: chime and spoken text, the chime alone, or nothing (the bus events still fire).
NOTIFY_SPEECH = "speech"
NOTIFY_TONE = "tone"
NOTIFY_NONE = "none"
NOTIFY_MODES = [NOTIFY_SPEECH, NOTIFY_TONE, NOTIFY_NONE]

# Services the display plugin (and anyone else) calls, each answering with a response.
SERVICE_OVERVIEW = "get_overview"
SERVICE_QUESTION = "get_question"
SERVICE_HINT = "get_hint"
SERVICE_ANSWER = "answer"
SERVICE_NOTIFY = "set_notify"
SERVICE_BACKGROUND = "get_background"

# Events on Home Assistant's bus, for automations (a chime of your own, a notification).
EVENT_QUESTION = f"{DOMAIN}_question"
EVENT_HINT = f"{DOMAIN}_hint"

PRUNE_SECONDS = 10


def notify_of(options: dict) -> str:
    """The announce mode of some options: the stored one, else what the old `speak` yes/no meant."""
    mode = options.get(CONF_NOTIFY)
    if mode in NOTIFY_MODES:
        return mode
    return NOTIFY_TONE if options.get(CONF_SPEAK) is False else NOTIFY_SPEECH
