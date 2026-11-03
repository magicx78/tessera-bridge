"""The integration inside Home Assistant: the HTTP API the mod talks to, the sensors, the services the display calls.

    /path/to/venv/bin/python -m pytest tests/test_integration.py   (needs pytest-homeassistant-custom-component)
"""
from __future__ import annotations

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.tessera_claude.const import DOMAIN

pytestmark = pytest.mark.asyncio


def question(tool_use_id: str, nonce: str = "n1"):
    return {
        "id": tool_use_id,
        "nonce": nonce,
        "parts": [
            {
                "id": "p0",
                "header": "Weiter",
                "text": "Weiter machen?",
                "multi": False,
                "options": [{"id": "o0", "label": "Ja", "description": ""}, {"id": "o1", "label": "Nein", "description": ""}],
            }
        ],
    }


async def setup_entry(hass):
    entry = MockConfigEntry(domain=DOMAIN, data={}, options={})
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


async def test_reports_become_sensors_and_the_display_answers(hass, hass_client):
    await setup_entry(hass)
    client = await hass_client()

    # Two sessions report, one with a question.
    r = await client.post("/api/tessera_claude/v1/report", json={"instance": "pc1", "session": "a", "seq": 1, "name": "Garden", "state": "asking", "questions": [question("toolu_1")], "context": {"percent": 11}, "limits": [{"kind": "five_hour", "percent": 44, "resets_at": "2099-01-01T00:00:00Z"}]})
    assert r.status == 200
    handle = (await r.json())["questions"]["toolu_1"]["handle"]
    r = await client.post("/api/tessera_claude/v1/report", json={"instance": "pc1", "session": "b", "seq": 1, "name": "Shop", "state": "working", "questions": [], "context": {"percent": 42}})
    assert r.status == 200
    await hass.async_block_till_done()

    state = hass.states.get("sensor.claude_sessions")
    assert state is not None and state.state == "2"
    assert sorted(state.attributes["names"]) == ["Garden", "Shop"]
    assert state.attributes["open"] == 1
    assert state.attributes["event"].startswith("question:")
    assert state.attributes["l5h"] == 44
    assert hass.states.get("sensor.claude_questions").state == "1"
    assert hass.states.get("sensor.claude_limit_5h").state == "44"
    assert hass.states.get("sensor.claude_limit_week").state == "unknown"

    # The display reads the question and answers it; the bridge's next report learns of it.
    detail = await hass.services.async_call(DOMAIN, "get_question", {"question": handle}, blocking=True, return_response=True)
    assert detail["parts"][0]["options"][1]["id"] == "o1"
    reply = await hass.services.async_call(DOMAIN, "answer", {"question": handle, "rev": detail["rev"], "answers": {"p0": ["o1"]}, "display": "kitchen"}, blocking=True, return_response=True)
    assert reply == {"accepted": True}
    again = await hass.services.async_call(DOMAIN, "answer", {"question": handle, "answers": {"p0": ["o0"]}}, blocking=True, return_response=True)
    assert again["accepted"] is False
    r = await client.post("/api/tessera_claude/v1/report", json={"instance": "pc1", "session": "a", "seq": 2, "name": "Garden", "state": "asking", "questions": [question("toolu_1")]})
    status = (await r.json())["questions"]["toolu_1"]
    assert status["status"] == "answered" and status["by"] == "display" and status["answer"] == {"p0": ["o1"]}
    # The PC comes after: refused, the display's answer handed back.
    r = await client.post("/api/tessera_claude/v1/claim", json={"instance": "pc1", "session": "a", "id": "toolu_1", "nonce": "n1", "by": "pc", "outcome": "answered"})
    claim = await r.json()
    assert claim["accepted"] is False and claim["answer"] == {"p0": ["o1"]}

    overview = await hass.services.async_call(DOMAIN, "get_overview", {}, blocking=True, return_response=True)
    assert len(overview["sessions"]) == 2
    assert overview["limits"]["kinds"]["five_hour"]["percent"] == 44

    # The session ends; its row goes.
    r = await client.post("/api/tessera_claude/v1/end", json={"instance": "pc1", "session": "a", "reason": "prompt_input_exit"})
    assert (await r.json()) == {"ok": True}
    await hass.async_block_till_done()
    assert hass.states.get("sensor.claude_sessions").state == "1"


async def test_the_api_refuses_without_a_token_and_bad_bodies(hass, hass_client_no_auth, hass_client):
    await setup_entry(hass)
    anonymous = await hass_client_no_auth()
    r = await anonymous.post("/api/tessera_claude/v1/report", json={})
    assert r.status == 401
    client = await hass_client()
    r = await client.post("/api/tessera_claude/v1/nothing", json={})
    assert r.status == 404
    r = await client.post("/api/tessera_claude/v1/report", data="not json", headers={"Content-Type": "application/json"})
    assert r.status == 400
    r = await client.post("/api/tessera_claude/v1/report", json={"seq": 1})
    assert (await r.json())["ok"] is False


async def test_unload_cleans_up(hass):
    entry = await setup_entry(hass)
    assert hass.services.has_service(DOMAIN, "answer")
    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    assert not hass.services.has_service(DOMAIN, "answer")
