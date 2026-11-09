"""The HTTP API the tessera-bridge mod talks to: POST /api/tessera_claude/v1/{report|claim|end}.

Signed in with a long-lived access token, as any call to Home Assistant's /api/. Every body and answer is JSON
(docs/PROTOCOL.md in the repository).
"""
from __future__ import annotations

import logging
import time

from aiohttp import web
from homeassistant.components.http import KEY_HASS_USER, HomeAssistantView
from homeassistant.core import HomeAssistant

from .access import async_is_allowed
from .const import API_PATH, DOMAIN

_LOGGER = logging.getLogger(__name__)

ACTIONS = ("report", "claim", "end")


class TesseraClaudeView(HomeAssistantView):
    url = API_PATH
    name = "api:tessera_claude:v1"
    requires_auth = True

    def __init__(self, hass: HomeAssistant) -> None:
        self.hass = hass

    async def post(self, request: web.Request, action: str) -> web.Response:
        if action not in ACTIONS:
            return self.json_message("unknown action", status_code=404)
        entry = self.hass.data.get(DOMAIN, {}).get("entry")
        if entry is None or not hasattr(entry, "runtime_data"):
            return self.json_message("tessera_claude is not set up", status_code=503)
        user = request.get(KEY_HASS_USER)
        if user is None:  # authenticated but without a user: refused whatever ALLOW_CALLS_WITHOUT_USER says
            return self.json_message("no user", status_code=401)
        if not await async_is_allowed(self.hass, entry, user.id):
            return self.json_message("user not allowed", status_code=403)
        try:
            body = await request.json()
        except ValueError:
            return self.json_message("invalid json", status_code=400)
        if not isinstance(body, dict):
            return self.json_message("invalid body", status_code=400)
        registry = entry.runtime_data.registry
        now = time.monotonic()
        if action == "report":
            if _LOGGER.isEnabledFor(logging.DEBUG):
                limits = body.get("limits") if isinstance(body.get("limits"), list) else []
                _LOGGER.debug(
                    "report %s/%s state=%s limits_age=%s kinds=%s",
                    body.get("instance"),
                    str(body.get("session"))[:8],
                    body.get("state"),
                    body.get("limits_age"),
                    [(l.get("kind"), l.get("percent")) for l in limits if isinstance(l, dict)],
                )
            reply = registry.report(body, now)
        elif action == "claim":
            reply = registry.claim(body)
        else:
            reply = registry.end(body, now)
        return self.json(reply)
