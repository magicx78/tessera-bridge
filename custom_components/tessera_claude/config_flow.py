"""The config flow: one entry, with the optional announce and hint settings, changeable later under Options."""
from __future__ import annotations

from typing import Any

import voluptuous as vol
from homeassistant.config_entries import (
    ConfigEntry,
    ConfigFlow,
    ConfigFlowResult,
    OptionsFlow,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import selector

from .access import allowed_users_of
from .const import (
    CONF_ALLOWED_USERS,
    CONF_CHIME,
    CONF_HINTS,
    CONF_LANGUAGE,
    CONF_NOTIFY,
    CONF_PLAYER,
    CONF_TTS,
    DEFAULT_CHIME,
    DEFAULT_LANGUAGE,
    DOMAIN,
    NOTIFY_MODES,
    notify_of,
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
            vol.Optional(CONF_NOTIFY, default=notify_of(current)): selector.SelectSelector(
                selector.SelectSelectorConfig(
                    options=NOTIFY_MODES, translation_key=CONF_NOTIFY, mode=selector.SelectSelectorMode.DROPDOWN
                )
            ),
            vol.Optional(CONF_HINTS, default=current.get(CONF_HINTS, True)): bool,
        }
    )


async def _user_options(hass: HomeAssistant, current: list[str]) -> list[selector.SelectOptionDict]:
    users = [u for u in await hass.auth.async_get_users() if not u.system_generated]
    options = [selector.SelectOptionDict(value=u.id, label=u.name or u.id) for u in users]
    known = {u.id for u in users}
    # An id kept in the list whose user is gone stays offered, else saving the form would fail on it.
    options += [selector.SelectOptionDict(value=i, label=i) for i in current if i not in known]
    return options


class TesseraClaudeConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 2  # 2: the yes/no `speak` became the three-way `notify` (see async_migrate_entry)

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
            # The frontend leaves out a field that was emptied: that means "everyone", not the old list.
            user_input.setdefault(CONF_ALLOWED_USERS, [])
            return self.async_create_entry(title="", data=user_input)
        current = dict(self.config_entry.options)
        allowed = allowed_users_of(current)
        schema = _schema(current).extend(
            {
                vol.Optional(CONF_ALLOWED_USERS, description={"suggested_value": allowed}): selector.SelectSelector(
                    selector.SelectSelectorConfig(
                        options=await _user_options(self.hass, allowed),
                        multiple=True,
                        mode=selector.SelectSelectorMode.DROPDOWN,
                    )
                )
            }
        )
        return self.async_show_form(step_id="init", data_schema=schema)
