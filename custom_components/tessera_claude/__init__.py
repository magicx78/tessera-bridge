"""Tessera Claude: every Claude session, its questions and the account's limits, for Tessera screens.

The tessera-bridge mod in each Claude session reports to the HTTP API (api.py); the registry (registry.py) keeps
the state and settles who answered a question first; the sensors (sensor.py) show it; the services (services.py)
answer the display plugin; sound.py says a new question or a hint through Chime TTS when a player is set.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import timedelta
import time

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.event import async_track_time_interval

from .api import TesseraClaudeView
from .const import CONF_NOTIFY, CONF_SPEAK, DOMAIN, PRUNE_SECONDS, SIGNAL_UPDATE, notify_of
from .registry import Registry
from .services import async_setup_services, async_unload_services
from .sound import Announcer

PLATFORMS = [Platform.SENSOR]


@dataclass
class TesseraClaudeData:
    registry: Registry
    announcer: Announcer
    unsubscribe: list


type TesseraClaudeConfigEntry = ConfigEntry[TesseraClaudeData]


def now() -> float:
    return time.monotonic()


async def async_setup_entry(hass: HomeAssistant, entry: TesseraClaudeConfigEntry) -> bool:
    registry = Registry()
    announcer = Announcer(hass, entry)
    data = TesseraClaudeData(registry=registry, announcer=announcer, unsubscribe=[])
    entry.runtime_data = data

    @callback
    def changed() -> None:
        async_dispatcher_send(hass, SIGNAL_UPDATE)
        for event in registry.take_events():
            hass.async_create_task(announcer.handle(event))

    data.unsubscribe.append(registry.listen(changed))

    async def prune(_now) -> None:
        if registry.prune(now()):
            return
        # A session that went quiet shows as lost without any report: the sensors read the clock.
        async_dispatcher_send(hass, SIGNAL_UPDATE)

    data.unsubscribe.append(async_track_time_interval(hass, prune, timedelta(seconds=PRUNE_SECONDS)))

    # One view for every config entry; the registry it reaches is looked up at each request.
    if not hass.data.get(DOMAIN, {}).get("view"):
        hass.data.setdefault(DOMAIN, {})["view"] = True
        hass.http.register_view(TesseraClaudeView(hass))
    hass.data[DOMAIN]["entry"] = entry

    await async_setup_services(hass, entry)
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    entry.async_on_unload(entry.add_update_listener(_options_changed))
    return True


async def _options_changed(hass: HomeAssistant, entry: TesseraClaudeConfigEntry) -> None:
    entry.runtime_data.announcer.reload()
    # The sessions sensor shows the announce mode (`notify`): it follows the change without waiting for a report.
    async_dispatcher_send(hass, SIGNAL_UPDATE)


async def async_migrate_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Version 1 kept a yes/no `speak`; version 2 keeps the three-way `notify` (speech, tone, none)."""
    if entry.version < 2:
        options = dict(entry.options)
        options[CONF_NOTIFY] = notify_of({**entry.data, **options})
        options.pop(CONF_SPEAK, None)
        hass.config_entries.async_update_entry(entry, options=options, version=2)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: TesseraClaudeConfigEntry) -> bool:
    ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if ok:
        for unsub in entry.runtime_data.unsubscribe:
            unsub()
        entry.runtime_data.unsubscribe.clear()
        async_unload_services(hass)
        hass.data.get(DOMAIN, {}).pop("entry", None)
    return ok
