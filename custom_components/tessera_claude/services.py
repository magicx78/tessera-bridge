"""The services the display plugin calls, each answering with a response (`return_response`).

The Tessera plugin asks them through `tessera::send` (`call_service:tessera_claude.<service>`); the Tessera app
calls them with `return_response` and hands the plugin the answer. Who else may call them is the option
`allowed_users` (access.py): empty, everyone; set, the listed users and administrators; calls without a user pass.
"""
from __future__ import annotations

import time

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import (
    HomeAssistant,
    ServiceCall,
    ServiceResponse,
    SupportsResponse,
)
from homeassistant.exceptions import Unauthorized

from .access import async_is_allowed
from .const import (
    CONF_NOTIFY,
    DOMAIN,
    NOTIFY_MODES,
    SERVICE_ANSWER,
    SERVICE_BACKGROUND,
    SERVICE_HINT,
    SERVICE_NOTIFY,
    SERVICE_OVERVIEW,
    SERVICE_QUESTION,
)
from .sensor import background_rows

ANSWER_SCHEMA = vol.Schema(
    {
        vol.Required("question"): str,
        vol.Optional("rev"): vol.Coerce(int),
        vol.Required("answers"): dict,
        vol.Optional("display", default=""): str,
    },
    extra=vol.ALLOW_EXTRA,
)
QUESTION_SCHEMA = vol.Schema({vol.Required("question"): str}, extra=vol.ALLOW_EXTRA)
HINT_SCHEMA = vol.Schema({vol.Required("session"): str}, extra=vol.ALLOW_EXTRA)
NOTIFY_SCHEMA = vol.Schema({vol.Required("mode"): vol.In(NOTIFY_MODES)})


async def async_setup_services(hass: HomeAssistant, entry: ConfigEntry) -> None:
    registry = entry.runtime_data.registry

    def guarded(handler):
        async def wrapper(call: ServiceCall) -> ServiceResponse:
            if not await async_is_allowed(hass, entry, call.context.user_id):
                raise Unauthorized(context=call.context, permission=DOMAIN)
            return await handler(call)

        return wrapper

    async def overview(call: ServiceCall) -> ServiceResponse:
        return registry.overview(time.monotonic())

    async def question(call: ServiceCall) -> ServiceResponse:
        return registry.question(call.data["question"])

    async def hint(call: ServiceCall) -> ServiceResponse:
        return registry.hint(call.data["session"], time.monotonic())

    async def answer(call: ServiceCall) -> ServiceResponse:
        return registry.answer(
            call.data["question"], call.data.get("rev"), call.data["answers"], call.data.get("display", "")
        )

    async def background(call: ServiceCall) -> ServiceResponse:
        return {"tasks": background_rows(registry)}

    async def set_notify(call: ServiceCall) -> ServiceResponse:
        # The options are the one place the mode lives; the update listener reloads the announcer and the sensor.
        mode = call.data["mode"]
        hass.config_entries.async_update_entry(entry, options={**entry.options, CONF_NOTIFY: mode})
        return {"notify": mode}

    hass.services.async_register(DOMAIN, SERVICE_OVERVIEW, guarded(overview), supports_response=SupportsResponse.ONLY)
    hass.services.async_register(
        DOMAIN, SERVICE_QUESTION, guarded(question), schema=QUESTION_SCHEMA, supports_response=SupportsResponse.ONLY
    )
    hass.services.async_register(DOMAIN, SERVICE_HINT, guarded(hint), schema=HINT_SCHEMA, supports_response=SupportsResponse.ONLY)
    hass.services.async_register(
        DOMAIN, SERVICE_ANSWER, guarded(answer), schema=ANSWER_SCHEMA, supports_response=SupportsResponse.OPTIONAL
    )
    hass.services.async_register(DOMAIN, SERVICE_BACKGROUND, guarded(background), supports_response=SupportsResponse.ONLY)
    hass.services.async_register(
        DOMAIN, SERVICE_NOTIFY, guarded(set_notify), schema=NOTIFY_SCHEMA, supports_response=SupportsResponse.OPTIONAL
    )


def async_unload_services(hass: HomeAssistant) -> None:
    for service in (
        SERVICE_OVERVIEW,
        SERVICE_QUESTION,
        SERVICE_HINT,
        SERVICE_ANSWER,
        SERVICE_BACKGROUND,
        SERVICE_NOTIFY,
    ):
        hass.services.async_remove(DOMAIN, service)
