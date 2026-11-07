"""The integration inside Home Assistant: the HTTP API the mod talks to, the sensors, the services the display calls.

    /path/to/venv/bin/python -m pytest tests/test_integration.py   (needs pytest-homeassistant-custom-component)
"""
from __future__ import annotations

import pytest
import voluptuous as vol
from pytest_homeassistant_custom_component.common import MockConfigEntry, async_capture_events, async_mock_service

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


def post_report(client, **more):
    body = {"instance": "pc1", "session": "a", "seq": 1, "name": "Garden", "state": "working", "questions": []}
    return client.post("/api/tessera_claude/v1/report", json={**body, **more})


async def setup_entry(hass, options=None, version=2):
    entry = MockConfigEntry(domain=DOMAIN, data={}, options=options or {}, version=version)
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

    # A background task of session b: the background sensor lists it with its start.
    r = await client.post("/api/tessera_claude/v1/report", json={"instance": "pc1", "session": "b", "seq": 2, "name": "Shop", "state": "working", "questions": [], "tasks": [{"id": "bg1", "kind": "shell", "title": "Build the firmware", "age": 60}]})
    assert r.status == 200
    await hass.async_block_till_done()
    background = hass.states.get("sensor.claude_background")
    assert background.state == "1"
    assert background.attributes["bg_titles"] == ["Build the firmware"]
    assert background.attributes["bg_sess"] == ["Shop"] and background.attributes["bg_kinds"] == ["shell"]
    assert len(background.attributes["bg_hm"][0]) == 5 and background.attributes["bg_at"][0] > 0

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
    for service in ("answer", "get_background", "set_notify"):
        assert hass.services.has_service(DOMAIN, service)
    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    assert not hass.services.has_service(DOMAIN, "answer")
    for service in ("get_background", "set_notify"):
        assert not hass.services.has_service(DOMAIN, service)


async def test_set_notify_answers_and_the_options_and_the_sensor_follow(hass):
    entry = await setup_entry(hass)
    assert hass.states.get("sensor.claude_sessions").attributes["notify"] == "speech"

    reply = await hass.services.async_call(DOMAIN, "set_notify", {"mode": "tone"}, blocking=True, return_response=True)
    assert reply == {"notify": "tone"}
    await hass.async_block_till_done()
    assert entry.options["notify"] == "tone"
    assert hass.states.get("sensor.claude_sessions").attributes["notify"] == "tone"
    assert entry.runtime_data.announcer.notify == "tone"

    # A mode that is none of the three is refused and changes nothing.
    with pytest.raises(vol.Invalid):
        await hass.services.async_call(DOMAIN, "set_notify", {"mode": "loud"}, blocking=True, return_response=True)
    assert entry.options["notify"] == "tone"


@pytest.mark.parametrize("mode", ["speech", "tone", "none"])
async def test_the_announcer_says_what_the_mode_allows(hass, hass_client, mode):
    calls = async_mock_service(hass, "chime_tts", "say")
    events = async_capture_events(hass, "tessera_claude_question")
    await setup_entry(hass, {"player": "media_player.kitchen", "notify": mode})
    client = await hass_client()

    r = await post_report(client, state="asking", questions=[question("toolu_1")])
    assert r.status == 200
    await hass.async_block_till_done()

    # The bus event fires in every mode: an automation of your own does not depend on the announcement.
    assert len(events) == 1 and events[0].data["text"] == "Weiter machen?"
    if mode == "none":
        assert calls == []
        return
    assert len(calls) == 1
    data = calls[0].data
    assert data["entity_id"] == "media_player.kitchen" and data["chime_path"] == "chirp"
    if mode == "speech":
        assert data["message"] == "Garden: Weiter machen?"
    else:
        assert "message" not in data


async def test_tone_mode_falls_back_to_the_default_chime(hass, hass_client):
    calls = async_mock_service(hass, "chime_tts", "say")
    await setup_entry(hass, {"player": "media_player.kitchen", "notify": "tone", "chime": ""})
    client = await hass_client()
    await post_report(client, state="asking", questions=[question("toolu_1")])
    await hass.async_block_till_done()
    assert len(calls) == 1
    assert calls[0].data["chime_path"] == "chirp" and "message" not in calls[0].data


async def test_a_hint_follows_the_mode(hass, hass_client):
    calls = async_mock_service(hass, "chime_tts", "say")
    hints = async_capture_events(hass, "tessera_claude_hint")
    entry = await setup_entry(hass, {"player": "media_player.kitchen", "notify": "none"})
    client = await hass_client()

    await post_report(client, state="permission", detail="Bash: ls")
    await hass.async_block_till_done()
    assert len(hints) == 1 and calls == []

    # Tone: the hint is a chime without text.
    await hass.services.async_call(DOMAIN, "set_notify", {"mode": "tone"}, blocking=True)
    await hass.async_block_till_done()
    await post_report(client, seq=2, state="permission", detail="Bash: rm")
    await hass.async_block_till_done()
    assert len(hints) == 2 and len(calls) == 1 and "message" not in calls[0].data
    assert entry.options["notify"] == "tone"


async def test_an_entry_not_migrated_yet_still_reads_the_old_speak(hass, hass_client):
    calls = async_mock_service(hass, "chime_tts", "say")
    # Version 2 with the old key: the announcer falls back from `speak` when `notify` is absent.
    await setup_entry(hass, {"player": "media_player.kitchen", "speak": False})
    client = await hass_client()
    await post_report(client, state="asking", questions=[question("toolu_1")])
    await hass.async_block_till_done()
    assert len(calls) == 1 and "message" not in calls[0].data
    assert hass.states.get("sensor.claude_sessions").attributes["notify"] == "tone"


@pytest.mark.parametrize(
    "old, expected",
    [({"speak": False, "hints": True}, "tone"), ({"speak": True}, "speech"), ({}, "speech")],
)
async def test_version_1_entries_migrate_speak_to_notify(hass, old, expected):
    entry = await setup_entry(hass, {"player": "media_player.kitchen", **old}, version=1)
    assert entry.version == 2
    assert entry.options["notify"] == expected
    assert "speak" not in entry.options
    # The other options stay as they were.
    assert entry.options["player"] == "media_player.kitchen"
    assert entry.options.get("hints", True) is True
    assert hass.states.get("sensor.claude_sessions").attributes["notify"] == expected


async def test_the_options_form_offers_notify_and_saves_it(hass):
    entry = await setup_entry(hass, {"notify": "tone"})
    form = await hass.config_entries.options.async_init(entry.entry_id)
    assert form["type"] == "form" and form["step_id"] == "init"
    keys = {str(key): key for key in form["data_schema"].schema}
    assert "notify" in keys and "speak" not in keys
    assert keys["notify"].default() == "tone"

    done = await hass.config_entries.options.async_configure(form["flow_id"], {"notify": "none", "hints": False})
    assert done["type"] == "create_entry"
    await hass.async_block_till_done()
    assert entry.options["notify"] == "none"
    assert hass.states.get("sensor.claude_sessions").attributes["notify"] == "none"


async def test_background_progress_reaches_the_sensor_and_the_service(hass, hass_client):
    await setup_entry(hass)
    client = await hass_client()

    task = {"id": "bg1", "kind": "agent", "title": "Write the tests", "age": 3600, "done": 2, "total": 5, "note": "Writing test_x"}
    await post_report(client, tasks=[task])
    await hass.async_block_till_done()
    attributes = hass.states.get("sensor.claude_background").attributes
    assert attributes["bg_done"] == [2] and attributes["bg_total"] == [5]

    reply = await hass.services.async_call(DOMAIN, "get_background", {}, blocking=True, return_response=True)
    (row,) = reply["tasks"]
    assert row["title"] == "Write the tests" and row["kind"] == "agent" and row["session"] == "Garden"
    assert (row["done"], row["total"], row["note"]) == (2, 5, "Writing test_x")
    # The same row the sensor lists: the start in epoch seconds and as the clock reads it.
    assert row["at"] == attributes["bg_at"][0] and row["hm"] == attributes["bg_hm"][0]
    assert isinstance(row["at"], int) and len(row["hm"]) == 5

    # A task that reports no progress: -1 and 0 (unknown), newest first.
    plain = {"id": "bg2", "kind": "shell", "title": "Build", "age": 0}
    await post_report(client, seq=2, tasks=[task, plain])
    await hass.async_block_till_done()
    attributes = hass.states.get("sensor.claude_background").attributes
    assert attributes["bg_titles"] == ["Build", "Write the tests"]
    assert attributes["bg_done"] == [-1, 2] and attributes["bg_total"] == [0, 5]
    reply = await hass.services.async_call(DOMAIN, "get_background", {}, blocking=True, return_response=True)
    assert [(t["title"], t["done"], t["total"], t["note"]) for t in reply["tasks"]] == [("Build", -1, 0, ""), ("Write the tests", 2, 5, "Writing test_x")]


async def test_get_background_is_empty_without_tasks(hass):
    await setup_entry(hass)
    reply = await hass.services.async_call(DOMAIN, "get_background", {}, blocking=True, return_response=True)
    assert reply == {"tasks": []}
