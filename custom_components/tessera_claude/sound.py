"""Sounds, optional: a chime and the spoken question or hint through Chime TTS, once per question or hint.

The `notify` option picks speech (chime and text), tone (the chime alone) or none. Nothing plays without a player in
the options. Every new question and hint also goes out as an event on the bus
(tessera_claude_question, tessera_claude_hint), for an automation of your own.
"""
from __future__ import annotations

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .const import (
    CONF_CHIME,
    CONF_HINTS,
    CONF_LANGUAGE,
    CONF_PLAYER,
    CONF_TTS,
    DEFAULT_CHIME,
    DEFAULT_LANGUAGE,
    EVENT_HINT,
    EVENT_QUESTION,
    NOTIFY_NONE,
    NOTIFY_SPEECH,
    NOTIFY_TONE,
    notify_of,
)

_LOGGER = logging.getLogger(__name__)


class Announcer:
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.hass = hass
        self.entry = entry
        self.reload()

    def reload(self) -> None:
        options = {**self.entry.data, **self.entry.options}
        self.player = options.get(CONF_PLAYER) or ""
        self.chime = options.get(CONF_CHIME, DEFAULT_CHIME) or ""
        self.tts = options.get(CONF_TTS) or ""
        self.language = options.get(CONF_LANGUAGE, DEFAULT_LANGUAGE) or ""
        # `notify_of` falls back to the old `speak` yes/no for an entry that was not migrated yet.
        self.notify = notify_of(options)
        self.hints = options.get(CONF_HINTS, True)

    async def handle(self, event: dict) -> None:
        kind = event.get("kind")
        if kind == "question":
            self.hass.bus.async_fire(EVENT_QUESTION, event)
            await self.say(f"{event.get('session', '')}: {event.get('text', '')}")
        elif kind == "hint":
            self.hass.bus.async_fire(EVENT_HINT, event)
            if self.hints:
                await self.say(f"{event.get('session', '')} wartet auf eine Freigabe am PC.")

    async def say(self, text: str) -> None:
        if not self.player or self.notify == NOTIFY_NONE:
            return
        data: dict = {"entity_id": self.player, "announce": True}
        chime = self.chime or (DEFAULT_CHIME if self.notify == NOTIFY_TONE else "")
        if chime:
            data["chime_path"] = chime
        if self.notify == NOTIFY_SPEECH and text:
            data["message"] = text
            if self.language:
                data["language"] = self.language
            if self.tts:
                data["tts_platform"] = self.tts
        if "chime_path" not in data and "message" not in data:
            return
        if not self.hass.services.has_service("chime_tts", "say"):
            _LOGGER.warning("tessera_claude: a player is set but the Chime TTS integration is not installed")
            return
        try:
            await self.hass.services.async_call("chime_tts", "say", data, blocking=False)
        except Exception as err:  # noqa: BLE001 - a sound never breaks a question
            _LOGGER.warning("tessera_claude: chime_tts.say failed: %s", err)
