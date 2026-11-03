"""The config flow: one entry, with the optional sound and hint settings, changeable later under Options."""
from __future__ import annotations

from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.core import callback
from homeassistant.helpers import selector

from .const import (
    CONF_CHIME,
    CONF_HINTS,
    CONF_LANGUAGE,
    CONF_PLAYER,
    CONF_SPEAK,
    CONF_TTS,
    DEFAULT_CHIME,
    DEFAULT_LANGUAGE,
    DOMAIN,
)


def _schema(current: dict[str, Any]) -> vol.Schema:
    return vol.Schema(
        {
            vol.Optional(CONF_PLAYER, description={"suggested_value": current.get(CONF_PLAYER)}): selector.EntitySelector(
                selector.EntitySelectorConfig(domain="media_player")
            ),
            vol.Optional(CONF_CHIME, default=current.get(CONF_CHIME, DEFAULT_CHIME)): str,
            vol.Optional(CONF_TTS, description={"suggested_value": current.get(CONF_TTS)}): selector.EntitySelector(
                selector.EntitySelectorConfig(domain="tts")
            ),
            vol.Optional(CONF_LANGUAGE, default=current.get(CONF_LANGUAGE, DEFAULT_LANGUAGE)): str,
            vol.Optional(CONF_SPEAK, default=current.get(CONF_SPEAK, True)): bool,
            vol.Optional(CONF_HINTS, default=current.get(CONF_HINTS, True)): bool,
        }
    )


class TesseraClaudeConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            return self.async_create_entry(title="Tessera Claude", data={}, options=user_input)
        return self.async_show_form(step_id="user", data_schema=_schema({}))

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return TesseraClaudeOptionsFlow()


class TesseraClaudeOptionsFlow(OptionsFlow):
    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            return self.async_create_entry(title="", data=user_input)
        return self.async_show_form(step_id="init", data_schema=_schema(dict(self.config_entry.options)))
