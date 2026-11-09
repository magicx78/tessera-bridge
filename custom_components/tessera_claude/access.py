"""Who may call the integration's services and HTTP API (the option `allowed_users`).

Stage 1: an empty list lets everyone in (as before); a list lets only the listed users in, plus administrators (they
can change the option anyway). A call without a user (automation, script, ESPHome device action) is allowed by
`ALLOW_CALLS_WITHOUT_USER`. Every distinct caller id is logged once with the user's name, so the ids the Tessera
screen and the Claude Code mod use can be read from the log and put into the list.
"""
from __future__ import annotations

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .const import CONF_ALLOWED_USERS, DOMAIN

_LOGGER = logging.getLogger(__name__)

# STAGE 2 SWITCH: set to False to refuse calls that carry no user (context.user_id is None). Stage 1 allows them.
ALLOW_CALLS_WITHOUT_USER = True


def _log_state(hass: HomeAssistant) -> dict:
    """What was logged already (per Home Assistant instance, gone with a restart)."""
    return hass.data.setdefault(f"{DOMAIN}_access_log", {"seen": set(), "denied": set(), "no_user": False})


def allowed_users_of(options: dict) -> list[str]:
    value = options.get(CONF_ALLOWED_USERS)
    return [str(v) for v in value] if isinstance(value, (list, tuple)) else []


async def async_is_allowed(hass: HomeAssistant, entry: ConfigEntry, user_id: str | None) -> bool:
    """Whether the caller may use the services and the HTTP API; logs each distinct caller id once."""
    logged = _log_state(hass)
    if user_id is None:
        if not logged["no_user"]:
            logged["no_user"] = True
            _LOGGER.info("Call without a user (automation, script or device action); allowed: %s", ALLOW_CALLS_WITHOUT_USER)
        return ALLOW_CALLS_WITHOUT_USER
    allowed = allowed_users_of(entry.options)
    user = await hass.auth.async_get_user(user_id)
    name = user.name if user else "unknown user"
    if user_id not in logged["seen"]:
        logged["seen"].add(user_id)
        _LOGGER.info(
            "Caller of tessera_claude: %s (user id %s)%s", name, user_id, ", administrator" if user and user.is_admin else ""
        )
    if not allowed or user_id in allowed or (user and user.is_admin):
        return True
    if user_id not in logged["denied"]:
        logged["denied"].add(user_id)
        _LOGGER.warning("tessera_claude: refused %s (user id %s); add the user to allowed_users", name, user_id)
    return False
