"""The sensors: the sessions (with the lists the display reads), the open questions, and the account's limits."""
from __future__ import annotations

from datetime import datetime, timezone
import time
from typing import Any

from homeassistant.components.sensor import SensorEntity, SensorStateClass
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import PERCENTAGE
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.util import dt as dt_util

from .const import DOMAIN, SIGNAL_UPDATE
from .registry import Registry

# The names give the entity ids the old mod wrote: sensor.claude_limit_5h, sensor.claude_limit_week, sensor.claude_5h_reset.
KNOWN_LIMITS = {"five_hour": ("claude_limit_5h", "Claude Limit 5h"), "seven_day": ("claude_limit_week", "Claude Limit Week")}


def _epoch(iso: str | None) -> int | None:
    if not iso:
        return None
    parsed = dt_util.parse_datetime(iso)
    if parsed is None:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return int(parsed.timestamp())


def format_remaining(resets_at: str | None, now: datetime) -> str:
    """"1h 09m", "2d 3h", "0m" or "–", as the old mod wrote it."""
    if not resets_at:
        return "–"
    parsed = dt_util.parse_datetime(resets_at)
    if parsed is None:
        return "–"
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    seconds = (parsed - now).total_seconds()
    if seconds <= 0:
        return "0m"
    minutes = int(seconds // 60)
    days, rest = divmod(minutes, 1440)
    hours, mins = divmod(rest, 60)
    if days > 0:
        return f"{days}d {hours}h"
    if hours > 0:
        return f"{hours}h {mins:02d}m"
    return f"{mins}m"


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    registry: Registry = entry.runtime_data.registry
    sensors = [
        SessionsSensor(registry),
        QuestionsSensor(registry),
        LimitSensor(registry, "five_hour"),
        LimitSensor(registry, "seven_day"),
        ResetSensor(registry),
        OtherLimitsSensor(registry),
    ]
    _drop_stale_states(hass, sensors)
    add(sensors)


@callback
def _drop_stale_states(hass: HomeAssistant, sensors: list[ClaudeSensor]) -> None:
    """A state under one of our entity ids that none of our entities wrote (the old mod's REST states, which a session
    still running the old mod writes again after every restart) makes Home Assistant refuse the entity ("already
    exists - ignoring") or give it a `_2` id: such a state goes before the entities are added. A state of another
    integration's entity under that id is left alone."""
    entities = er.async_get(hass)
    for sensor in sensors:
        entity_id = entities.async_get_entity_id("sensor", DOMAIN, sensor.unique_id) or f"sensor.{sensor.slug}"
        if hass.states.get(entity_id) is None:
            continue
        entry = entities.async_get(entity_id)
        if entry is not None and entry.platform != DOMAIN:
            continue
        hass.states.async_remove(entity_id)


class ClaudeSensor(SensorEntity):
    _attr_should_poll = False
    _attr_has_entity_name = False

    def __init__(self, registry: Registry, object_id: str, name: str) -> None:
        self.registry = registry
        self.slug = object_id
        self._attr_unique_id = f"{DOMAIN}_{object_id}"
        self._attr_name = name

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(async_dispatcher_connect(self.hass, SIGNAL_UPDATE, self._refresh))

    @callback
    def _refresh(self) -> None:
        self.async_write_ha_state()

    def limits(self) -> dict[str, Any]:
        return self.registry.limits_summary(time.monotonic())


class SessionsSensor(ClaudeSensor):
    """How many sessions live, and in its attributes what the display shows: parallel lists, bounded."""

    _attr_icon = "mdi:robot"
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, registry: Registry) -> None:
        super().__init__(registry, "claude_sessions", "Claude Sessions")

    @property
    def native_value(self) -> int:
        return self.registry.snapshot(time.monotonic())["count"]

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        snap = self.registry.snapshot(time.monotonic())
        snap.pop("count", None)
        kinds = self.limits().get("kinds", {})
        five = kinds.get("five_hour", {})
        week = kinds.get("seven_day", {})
        other = {k: v for k, v in kinds.items() if k not in KNOWN_LIMITS}
        snap.update(
            {
                "l5h": five.get("percent") if five.get("percent") is not None else -1,
                "l5h_at": _epoch(five.get("resets_at")) or 0,
                "lwk": week.get("percent") if week.get("percent") is not None else -1,
                "lwk_at": _epoch(week.get("resets_at")) or 0,
                "lx_names": [k.replace("seven_day_", "").replace("_", " ")[:16] for k in other],
                "lx": [v.get("percent") if v.get("percent") is not None else -1 for v in other.values()],
            }
        )
        return snap


class QuestionsSensor(ClaudeSensor):
    _attr_icon = "mdi:comment-question"
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, registry: Registry) -> None:
        super().__init__(registry, "claude_questions", "Claude Questions")

    @property
    def native_value(self) -> int:
        return self.registry.snapshot(time.monotonic())["open"]

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        now = time.monotonic()
        rows = []
        for session in self.registry.sessions.values():
            for question in self.registry.open_questions(session):
                rows.append({"question": question.handle, "session": session.name, "text": question.parts[0]["text"]})
        return {"questions": rows, "event": self.registry.event, "event_rev": self.registry.event_rev, "now": int(now)}


class LimitSensor(ClaudeSensor):
    """One of the account's rate-limit windows, in percent used; unknown while no session reported one."""

    _attr_icon = "mdi:gauge"
    _attr_native_unit_of_measurement = PERCENTAGE

    def __init__(self, registry: Registry, kind: str) -> None:
        object_id, name = KNOWN_LIMITS[kind]
        super().__init__(registry, object_id, name)
        self.kind = kind

    def _limit(self) -> dict[str, Any]:
        return self.limits().get("kinds", {}).get(self.kind, {})

    @property
    def native_value(self) -> int | None:
        return self._limit().get("percent")

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        limit = self._limit()
        return {
            "kind": self.kind,
            "resets_at": limit.get("resets_at") or "",
            "resets_in": format_remaining(limit.get("resets_at"), dt_util.utcnow()),
        }


class ResetSensor(ClaudeSensor):
    """When the five-hour window resets, as words ("1h 09m"), as the old sensor.claude_5h_reset."""

    _attr_icon = "mdi:timer-outline"

    def __init__(self, registry: Registry) -> None:
        super().__init__(registry, "claude_5h_reset", "Claude 5h Reset")

    @property
    def native_value(self) -> str:
        limit = self.limits().get("kinds", {}).get("five_hour", {})
        return format_remaining(limit.get("resets_at"), dt_util.utcnow())

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        limit = self.limits().get("kinds", {}).get("five_hour", {})
        return {"resets_at": limit.get("resets_at") or ""}


class OtherLimitsSensor(ClaudeSensor):
    """Every other window the account reports (a model's weekly window), one attribute each."""

    _attr_icon = "mdi:gauge-low"

    def __init__(self, registry: Registry) -> None:
        super().__init__(registry, "claude_limits", "Claude Limits")

    @property
    def native_value(self) -> int:
        return len([k for k in self.limits().get("kinds", {}) if k not in KNOWN_LIMITS])

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        now = dt_util.utcnow()
        attrs: dict[str, Any] = {}
        for kind, limit in self.limits().get("kinds", {}).items():
            if kind in KNOWN_LIMITS:
                continue
            attrs[kind] = limit.get("percent")
            attrs[f"{kind}_resets_in"] = format_remaining(limit.get("resets_at"), now)
        return attrs
