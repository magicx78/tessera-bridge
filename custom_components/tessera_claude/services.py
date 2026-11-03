"""The services the display plugin calls, each answering with a response (`return_response`).

The Tessera plugin asks them through `tessera::send` (`call_service:tessera_claude.<service>`); the Tessera app
calls them with `return_response` and hands the plugin the answer. Anyone else may call them too.
"""
from __future__ import annotations

import time

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall, ServiceResponse, SupportsResponse

from .const import DOMAIN, SERVICE_ANSWER, SERVICE_HINT, SERVICE_OVERVIEW, SERVICE_QUESTION

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


async def async_setup_services(hass: HomeAssistant, entry: ConfigEntry) -> None:
    registry = entry.runtime_data.registry

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

    hass.services.async_register(DOMAIN, SERVICE_OVERVIEW, overview, supports_response=SupportsResponse.ONLY)
    hass.services.async_register(
        DOMAIN, SERVICE_QUESTION, question, schema=QUESTION_SCHEMA, supports_response=SupportsResponse.ONLY
    )
    hass.services.async_register(DOMAIN, SERVICE_HINT, hint, schema=HINT_SCHEMA, supports_response=SupportsResponse.ONLY)
    hass.services.async_register(
        DOMAIN, SERVICE_ANSWER, answer, schema=ANSWER_SCHEMA, supports_response=SupportsResponse.OPTIONAL
    )


def async_unload_services(hass: HomeAssistant) -> None:
    for service in (SERVICE_OVERVIEW, SERVICE_QUESTION, SERVICE_HINT, SERVICE_ANSWER):
        hass.services.async_remove(DOMAIN, service)
