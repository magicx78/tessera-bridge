"""The HTTP API the tessera-bridge mod talks to: POST /api/tessera_claude/v1/{report|claim|end}.

Signed in with a long-lived access token, as any call to Home Assistant's /api/. Every body and answer is JSON
(docs/PROTOCOL.md in the repository).
"""
from __future__ import annotations

import time

from aiohttp import web
from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant

from .const import API_PATH, DOMAIN

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
        try:
            body = await request.json()
        except ValueError:
            return self.json_message("invalid json", status_code=400)
        if not isinstance(body, dict):
            return self.json_message("invalid body", status_code=400)
        registry = entry.runtime_data.registry
        now = time.monotonic()
        if action == "report":
            reply = registry.report(body, now)
        elif action == "claim":
            reply = registry.claim(body)
        else:
            reply = registry.end(body, now)
        return self.json(reply)
