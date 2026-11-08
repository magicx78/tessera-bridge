"""The registry's rules with several sessions, several displays and the race between the PC and a display.

    python3 -m pytest tests/test_registry.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "custom_components" / "tessera_claude"))

import pytest  # noqa: E402

from registry import FORGET_AFTER, LIST_LIMIT, LOST_AFTER, NOTE_LIMIT, Registry  # noqa: E402


def question(tool_use_id: str, nonce: str = "n1", multi: bool = False, parts: int = 1, labels=("Ja", "Nein")):
    return {
        "id": tool_use_id,
        "nonce": nonce,
        "parts": [
            {
                "id": f"p{i}",
                "header": "Weiter",
                "text": "Weiter machen?" if i == 0 else f"Teil {i}?",
                "multi": multi,
                "options": [{"id": f"o{k}", "label": label, "description": ""} for k, label in enumerate(labels)],
            }
            for i in range(parts)
        ],
    }


def report(instance: str, session: str, seq: int, name: str = "", state: str = "idle", questions=(), **more):
    return {
        "instance": instance,
        "session": session,
        "seq": seq,
        "name": name or session,
        "state": state,
        "questions": list(questions),
        **more,
    }


def test_three_sessions_are_listed_apart_and_updated_without_any_focus():
    r = Registry()
    r.report(report("pc1", "a", 1, "Garden", "working", context={"percent": 11}), now=0)
    r.report(report("pc1", "b", 1, "Shop", "idle", context={"percent": 42}), now=0)
    r.report(report("pc2", "a", 1, "Garden", "idle", context={"percent": 7}), now=0)  # same session id, other PC
    snap = r.snapshot(now=1)
    assert snap["count"] == 3
    assert sorted(snap["names"]) == ["Garden", "Garden", "Shop"]
    assert sorted(snap["ctx"]) == [7, 11, 42]
    # A background session's report changes only its own row.
    r.report(report("pc1", "b", 2, "Shop", "working", context={"percent": 43}), now=2)
    snap = r.snapshot(now=3)
    by_name = dict(zip(snap["h"], snap["ctx"]))
    assert sorted(by_name.values()) == [7, 11, 43]
    assert snap["states"].count("work") == 2


def test_two_questions_with_the_same_title_and_options_stay_apart():
    r = Registry()
    ra = r.report(report("pc1", "a", 1, "Garden", "asking", [question("toolu_1")]), now=0)
    rb = r.report(report("pc1", "b", 1, "Garden", "asking", [question("toolu_1")]), now=0)
    qa = ra["questions"]["toolu_1"]["handle"]
    qb = rb["questions"]["toolu_1"]["handle"]
    assert qa != qb
    assert r.snapshot(0)["open"] == 2
    # Answering B's on the display answers B's alone; A's stays open.
    assert r.answer(qb, r.question(qb)["rev"], {"p0": ["o1"]}) == {"accepted": True}
    assert r.report(report("pc1", "a", 2, "Garden", "asking", [question("toolu_1")]), now=1)["questions"]["toolu_1"]["status"] == "open"
    reply = r.report(report("pc1", "b", 2, "Garden", "asking", [question("toolu_1")]), now=1)["questions"]["toolu_1"]
    assert reply["status"] == "answered" and reply["by"] == "display" and reply["answer"] == {"p0": ["o1"]}


def test_answering_b_on_the_display_while_a_is_selected_on_the_pc():
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working"), now=0)
    rb = r.report(report("pc1", "b", 1, "B", "asking", [question("toolu_b")]), now=0)
    qb = rb["questions"]["toolu_b"]["handle"]
    assert r.answer(qb, None, {"p0": ["o0"]})["accepted"] is True
    # A's state is untouched, B's bridge learns the answer with its next poll.
    assert r.sessions["pc1.a"].state == "working"
    assert r.report(report("pc1", "b", 2, "B", "asking", [question("toolu_b")]), now=1)["questions"]["toolu_b"]["by"] == "display"


def test_exactly_one_answer_takes_effect_pc_display_and_two_displays():
    r = Registry()
    rb = r.report(report("pc1", "a", 1, "A", "asking", [question("toolu_1")]), now=0)
    q = rb["questions"]["toolu_1"]["handle"]
    # Display one answers first; display two and the PC come after.
    assert r.answer(q, None, {"p0": ["o0"]}, "kitchen") == {"accepted": True}
    second = r.answer(q, None, {"p0": ["o1"]}, "hall")
    assert second["accepted"] is False and second["reason"] == "answered"
    pc = r.claim({"instance": "pc1", "session": "a", "id": "toolu_1", "nonce": "n1", "by": "pc", "outcome": "answered"})
    assert pc["accepted"] is False and pc["by"] == "display" and pc["answer"] == {"p0": ["o0"]}
    # The other way round: the PC first, then the display is refused.
    r2 = Registry()
    rb = r2.report(report("pc1", "a", 1, "A", "asking", [question("toolu_2")]), now=0)
    q2 = rb["questions"]["toolu_2"]["handle"]
    assert r2.claim({"instance": "pc1", "session": "a", "id": "toolu_2", "nonce": "n1", "by": "pc", "outcome": "answered"})["accepted"] is True
    assert r2.answer(q2, None, {"p0": ["o0"]})["reason"] == "answered"
    # A double tap: the same display twice.
    assert r2.answer(q2, None, {"p0": ["o0"]})["accepted"] is False


def test_turn_complete_in_a_leaves_b_untouched():
    r = Registry()
    ra = r.report(report("pc1", "a", 1, "A", "asking", [question("toolu_a")]), now=0)
    rb = r.report(report("pc1", "b", 1, "B", "asking", [question("toolu_b")]), now=0)
    qa, qb = ra["questions"]["toolu_a"]["handle"], rb["questions"]["toolu_b"]["handle"]
    # A's turn ends: its bridge reports without the question.
    r.report(report("pc1", "a", 2, "A", "idle"), now=1)
    assert r.question(qa)["status"] == "cancelled"
    assert r.question(qb)["status"] == "open"
    assert r.snapshot(1)["open"] == 1
    # A late answer to A's question lands nowhere.
    assert r.answer(qa, None, {"p0": ["o0"]})["accepted"] is False


def test_duplicate_late_and_resent_messages():
    r = Registry()
    r.report(report("pc1", "a", 5, "A", "asking", [question("toolu_1")]), now=0)
    # A report from before (seq 3) arrives late: it changes nothing, and is answered as a poll.
    late = r.report(report("pc1", "a", 3, "A", "working"), now=1)
    assert late.get("stale") is True
    assert r.sessions["pc1.a"].state == "asking"
    assert r.snapshot(1)["open"] == 1
    # The same report twice (a retry): one question, not two.
    r.report(report("pc1", "a", 6, "A", "asking", [question("toolu_1")]), now=2)
    r.report(report("pc1", "a", 6, "A", "asking", [question("toolu_1")]), now=2)
    assert len([q for q in r.questions.values() if q.status == "open"]) == 1
    # A reconnect with another nonce for the same call: a new question under a new handle, the old one over.
    old = r.question(r.report(report("pc1", "a", 6, "A", "asking", [question("toolu_1")]), now=2)["questions"]["toolu_1"]["handle"])
    reply = r.report(report("pc1", "a", 7, "A", "asking", [question("toolu_1", nonce="n2")]), now=3)
    assert reply["questions"]["toolu_1"]["nonce"] == "n2"
    assert reply["questions"]["toolu_1"]["handle"] != old["h"]
    assert [q.status for q in r.questions.values()] == ["open"]
    # The display's answer names the old handle: refused.
    assert r.answer(old["h"], None, {"p0": ["o0"]})["accepted"] is False


def test_session_end_loss_and_restart_with_open_questions():
    r = Registry()
    rb = r.report(report("pc1", "a", 1, "A", "asking", [question("toolu_1")]), now=0)
    q = rb["questions"]["toolu_1"]["handle"]
    # The bridge goes quiet: the session shows as lost, the question stays until the session is forgotten.
    snap = r.snapshot(now=LOST_AFTER + 1)
    assert snap["states"] == ["lost"]
    assert r.answer(q, None, {"p0": ["o0"]})["accepted"] is True  # still answerable: the bridge may come back
    r.prune(now=FORGET_AFTER + 1)
    assert r.sessions == {}
    # A restart of Home Assistant: a fresh registry; the bridge's next report registers the question anew, under a
    # new handle, and the old answer (old handle) fits nothing.
    fresh = Registry()
    reply = fresh.report(report("pc1", "a", 9, "A", "asking", [question("toolu_1")]), now=0)
    assert reply["questions"]["toolu_1"]["status"] == "open"
    assert fresh.answer(q, None, {"p0": ["o0"]})["reason"] == "unknown"
    # The session ends: its questions are cancelled, the session shows as ended, then goes.
    fresh.end({"instance": "pc1", "session": "a"}, now=1)
    assert fresh.question(reply["questions"]["toolu_1"]["handle"])["status"] == "cancelled"
    assert fresh.snapshot(2)["count"] == 0
    fresh.prune(now=100)
    assert fresh.sessions == {}


def test_context_per_session_and_account_limits_without_double_counting():
    r = Registry()
    limits = [{"kind": "five_hour", "percent": 44, "resets_at": "2026-01-01T01:09:00Z"}, {"kind": "seven_day_opus", "percent": 10}]
    r.report(report("pc1", "a", 1, "A", "idle", context={"percent": 20}, limits=limits), now=0)
    r.report(report("pc1", "b", 1, "B", "idle", context={"percent": 70}, limits=limits), now=1)
    snap = r.snapshot(2)
    assert sorted(snap["ctx"]) == [20, 70]
    summary = r.limits_summary(2)
    assert summary["kinds"]["five_hour"]["percent"] == 44  # not 88
    assert summary["kinds"]["seven_day_opus"]["percent"] == 10
    # A session without a context says unknown (-1), never 0.
    r.report(report("pc1", "c", 1, "C", "idle"), now=3)
    assert -1 in r.snapshot(3)["ctx"]


def test_the_freshest_limits_reading_stands_and_an_empty_one_takes_nothing_away():
    r = Registry()
    fresh = [{"kind": "five_hour", "percent": 10}, {"kind": "seven_day", "percent": 33}]
    stale = [{"kind": "five_hour", "percent": 4}, {"kind": "seven_day", "percent": 31}]
    r.report(report("pc1", "a", 1, "A", "working", limits=fresh, limits_age=2), now=100)
    # An idle session's heartbeat repeats a reading it took an hour ago: it does not replace the fresh one.
    r.report(report("pc1", "b", 1, "B", "idle", limits=stale, limits_age=3600), now=101)
    assert r.limits_summary(102)["kinds"]["five_hour"]["percent"] == 10
    # A session with no reading yet reports an empty list: the known reading stays.
    r.report(report("pc1", "c", 1, "C", "idle", limits=[]), now=103)
    assert r.limits_summary(104)["kinds"]["seven_day"]["percent"] == 33
    # A newer measurement replaces it; a report without an age (an older mod) counts as measured now.
    r.report(report("pc1", "b", 2, "B", "working", limits=stale), now=105)
    assert r.limits_summary(106)["kinds"]["five_hour"]["percent"] == 4


def test_long_texts_several_options_and_paging_limits():
    r = Registry()
    long_text = "x" * 500
    q = question("toolu_1", labels=("Eins", "Zwei", "Drei", "Vier"), parts=4)
    q["parts"][0]["text"] = long_text
    reply = r.report(report("pc1", "a", 1, "A" * 100, "asking", [q]), now=0)
    detail = r.question(reply["questions"]["toolu_1"]["handle"])
    assert len(detail["parts"]) == 4 and len(detail["parts"][0]["options"]) == 4
    assert detail["parts"][0]["text"] == long_text  # whole here; the plugin cuts it into lines of 48 bytes
    assert len(detail["name"]) <= 24
    # More sessions than the lists carry: the lists stop, the count does not.
    for i in range(LIST_LIMIT + 3):
        r.report(report("pc1", f"s{i}", 1, f"S{i}", "idle"), now=0)
    snap = r.snapshot(0)
    assert len(snap["h"]) == LIST_LIMIT
    assert snap["count"] == LIST_LIMIT + 4
    # Asking sessions come first in the list.
    assert snap["states"][0] == "ask"


def test_multi_select_and_several_parts_are_checked():
    r = Registry()
    q = question("toolu_1", multi=True, parts=2, labels=("A", "B", "C"))
    q["parts"][1]["multi"] = False
    reply = r.report(report("pc1", "a", 1, "A", "asking", [q]), now=0)
    h = reply["questions"]["toolu_1"]["handle"]
    assert r.answer(h, None, {"p0": ["o0"]})["reason"] == "bad_answer"  # part 1 missing
    assert r.answer(h, None, {"p0": ["o0", "o2"], "p1": ["o0", "o1"]})["reason"] == "bad_answer"  # p1 takes one
    assert r.answer(h, None, {"p0": ["o0", "o2"], "p1": ["o9"]})["reason"] == "bad_answer"  # no such option
    assert r.answer(h, None, {"p0": ["o0", "o2", "o0"], "p1": "o1"}) == {"accepted": True}
    assert r.questions[f"pc1.a|toolu_1"].answer == {"p0": ["o0", "o2"], "p1": ["o1"]}


def test_no_sessions_unknown_usage_and_events():
    r = Registry()
    snap = r.snapshot(0)
    assert snap["count"] == 0 and snap["open"] == 0 and snap["h"] == []
    assert r.limits_summary(0) == {}
    assert r.overview(0) == {"sessions": [], "limits": {}}
    # A new question and a new hint each raise one event, once.
    r.report(report("pc1", "a", 1, "A", "asking", [question("toolu_1")]), now=0)
    r.report(report("pc1", "a", 2, "A", "asking", [question("toolu_1")]), now=1)
    events = r.take_events()
    assert [e["kind"] for e in events] == ["question"]
    r.report(report("pc1", "b", 1, "B", "permission", detail="Bash: rm -rf build"), now=2)
    r.report(report("pc1", "b", 2, "B", "permission", detail="Bash: rm -rf build"), now=3)
    assert [e["kind"] for e in r.take_events()] == ["hint"]
    assert r.event.startswith("hint:")
    hint = r.hint(r.sessions["pc1.b"].handle, 3)
    assert hint["detail"] == "Bash: rm -rf build" and hint["state"] == "perm"


def test_a_stale_rev_is_refused():
    r = Registry()
    reply = r.report(report("pc1", "a", 1, "A", "asking", [question("toolu_1")]), now=0)
    h = reply["questions"]["toolu_1"]["handle"]
    rev = r.question(h)["rev"]
    assert r.answer(h, rev - 1, {"p0": ["o0"]})["reason"] == "stale"
    assert r.answer(h, rev, {"p0": ["o0"]})["accepted"] is True



def test_active_since_is_the_oldest_unbroken_activity_and_last_active_stays_after_it():
    r = Registry()
    snap = r.snapshot(0)
    assert snap["active"] == 0 and snap["active_since"] == 0 and snap["last_active"] == 0
    r.report(report("pc1", "a", 1, "A", "idle"), now=10)
    assert r.snapshot(10)["active_since"] == 0
    # A starts working at 20, B at 30: the activity runs since A's start.
    r.report(report("pc1", "a", 2, "A", "working"), now=20)
    r.report(report("pc1", "b", 1, "B", "working"), now=30)
    # Working, then asking, then waiting for approval is one unbroken activity: the start stays.
    r.report(report("pc1", "a", 3, "A", "asking", [question("toolu_1")]), now=40)
    # The first open question of each row, for a tap that opens it.
    snap = r.snapshot(40)
    assert snap["q"][snap["names"].index("A")] == r.open_questions(r.sessions["pc1.a"])[0].handle
    assert snap["q"][snap["names"].index("B")] == ""
    r.report(report("pc1", "a", 4, "A", "permission"), now=50)
    snap = r.snapshot(50)
    assert snap["active"] == 2 and snap["active_since"] == 20 and snap["last_active"] == 50
    # A goes idle at 60: B's start stands.
    r.report(report("pc1", "a", 5, "A", "idle"), now=60)
    snap = r.snapshot(60)
    assert snap["active"] == 1 and snap["active_since"] == 30
    # B goes idle at 70: nothing is active, the last activity was at 70.
    r.report(report("pc1", "b", 2, "B", "idle"), now=70)
    snap = r.snapshot(80)
    assert snap["active"] == 0 and snap["active_since"] == 0 and snap["last_active"] == 70
    # A works again: a new start, not the old one.
    r.report(report("pc1", "a", 6, "A", "working"), now=90)
    assert r.snapshot(90)["active_since"] == 90


def test_a_lost_or_ended_session_is_not_active_and_the_last_activity_outlives_it():
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working"), now=5)
    # The bridge goes quiet: the session is lost, not active; its last word counts as its last activity.
    snap = r.snapshot(LOST_AFTER + 6)
    assert snap["active"] == 0 and snap["active_since"] == 0 and snap["last_active"] == 5
    r.report(report("pc1", "b", 1, "B", "working"), now=100)
    r.end({"instance": "pc1", "session": "b"}, now=110)
    snap = r.snapshot(111)
    assert snap["active"] == 0 and snap["last_active"] == 110
    # Both sessions are forgotten: the time of the last activity stays.
    r.prune(now=FORGET_AFTER + 200)
    assert r.sessions == {}
    assert r.snapshot(FORGET_AFTER + 200)["last_active"] == 110


def test_lines_stay_under_the_screens_48_bytes():
    from registry import wrap_lines

    lines = wrap_lines("Sollen die Änderungen an der Küchenbeleuchtung jetzt übernommen werden, oder lieber später?")
    assert all(len(line.encode("utf-8")) <= 46 for line in lines)
    assert " ".join(lines) == "Sollen die Änderungen an der Küchenbeleuchtung jetzt übernommen werden, oder lieber später?"
    long = wrap_lines("wort " * 200)
    assert len(long) == 6 and long[-1].endswith("…")
    assert wrap_lines("x" * 100)[0] and all(len(l.encode()) <= 46 for l in wrap_lines("x" * 100))
    assert wrap_lines("") == []


def test_background_tasks_keep_their_start_and_go_with_their_session():
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working", tasks=[{"id": "b1", "kind": "shell", "title": "Build the firmware", "age": 30}]), now=100)
    r.report(report("pc1", "b", 1, "B", "idle", tasks=[{"id": "a1", "kind": "agent", "title": "x" * 200, "age": 0}, {"id": "", "kind": "shell"}]), now=110)
    rows = r.background(110)
    # Newest first; an unknown kind is a shell, a long title is cut, a task without an id is dropped.
    assert [(t["id"], t["session"], t["started"]) for t in rows] == [("a1", "B", 110), ("b1", "A", 70)]
    assert len(rows[0]["title"]) == 48 and rows[1]["kind"] == "shell"
    # A later report with a rounded age does not move the start.
    r.report(report("pc1", "a", 2, "A", "working", tasks=[{"id": "b1", "kind": "shell", "title": "Build the firmware", "age": 46}]), now=115)
    assert [t["started"] for t in r.background(115) if t["id"] == "b1"] == [70]
    # The task ends: it goes. A lost session's tasks are not shown, an ended one's neither.
    r.report(report("pc1", "a", 3, "A", "idle", tasks=[]), now=120)
    assert [t["id"] for t in r.background(120)] == ["a1"]
    assert r.background(110 + LOST_AFTER + 1) == []
    r.report(report("pc1", "b", 2, "B", "idle", tasks=[{"id": "a1", "kind": "agent", "title": "t", "age": 0}]), now=200)
    r.end({"instance": "pc1", "session": "b"}, now=201)
    assert r.background(202) == []


def test_background_progress_lands_in_the_rows_and_follows_every_report():
    r = Registry()
    task = {"id": "a1", "kind": "agent", "title": "Write the tests", "age": 30, "done": 1, "total": 5, "note": "Reading the plan"}
    r.report(report("pc1", "a", 1, "A", "working", tasks=[task]), now=100)
    row = r.background(100)[0]
    assert (row["done"], row["total"], row["note"], row["started"]) == (1, 5, "Reading the plan", 70)
    # A later report moves the progress and the note, never the start (the age it carries is rounded).
    r.report(report("pc1", "a", 2, "A", "working", tasks=[{**task, "age": 41, "done": 3, "note": "Writing"}]), now=111)
    row = r.background(111)[0]
    assert (row["done"], row["total"], row["note"], row["started"]) == (3, 5, "Writing", 70)
    # A report without progress takes it back: the task is no longer known to be at a step.
    r.report(report("pc1", "a", 3, "A", "working", tasks=[{"id": "a1", "kind": "agent", "title": "Write the tests"}]), now=112)
    row = r.background(112)[0]
    assert (row["done"], row["total"], row["note"], row["started"]) == (None, None, "", 70)


@pytest.mark.parametrize(
    "done, total",
    [
        (6, 5),  # more done than the total
        (-1, 5),
        (2, 0),  # a total below 1
        (True, 5),  # a bool is not a count
        (2, True),
        (2.0, 5),  # nor is a float
        (2, 5.0),
        ("2", 5),
        (None, 5),
        (2, None),
    ],
)
def test_background_progress_that_is_not_sound_is_unknown(done, total):
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working", tasks=[{"id": "a1", "kind": "agent", "title": "t", "done": done, "total": total}]), now=100)
    row = r.background(100)[0]
    assert row["done"] is None and row["total"] is None


def test_background_progress_edges_and_missing_fields_are_known_or_unknown():
    r = Registry()
    tasks = [
        {"id": "start", "title": "t", "done": 0, "total": 1},
        {"id": "end", "title": "t", "done": 4, "total": 4},
        {"id": "plain", "title": "t"},
    ]
    r.report(report("pc1", "a", 1, "A", "working", tasks=tasks), now=100)
    by_id = {t["id"]: t for t in r.background(100)}
    assert (by_id["start"]["done"], by_id["start"]["total"]) == (0, 1)
    assert (by_id["end"]["done"], by_id["end"]["total"]) == (4, 4)
    assert (by_id["plain"]["done"], by_id["plain"]["total"], by_id["plain"]["note"]) == (None, None, "")


@pytest.mark.parametrize("calls, kept", [(0, 0), (17, 17), (-1, None), (True, None), (3.0, None), ("3", None), (None, None)])
def test_background_tool_calls_are_a_count_or_unknown(calls, kept):
    """An agent that reports no step still shows that it works: the mod counts its tool calls."""
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working", tasks=[{"id": "a1", "kind": "agent", "title": "t", "calls": calls}]), now=100)
    assert r.background(100)[0]["calls"] == kept
    r.report(report("pc1", "a", 2, "A", "working", tasks=[{"id": "a1", "kind": "agent", "title": "t"}]), now=101)
    assert r.background(101)[0]["calls"] is None


def test_background_note_is_cut_and_only_text():
    r = Registry()
    tasks = [{"id": "long", "title": "t", "note": "n" * 200}, {"id": "odd", "title": "t", "note": 7}]
    r.report(report("pc1", "a", 1, "A", "working", tasks=tasks), now=100)
    by_id = {t["id"]: t for t in r.background(100)}
    assert NOTE_LIMIT == 40 and len(by_id["long"]["note"]) == NOTE_LIMIT and by_id["long"]["note"].endswith("…")
    assert by_id["odd"]["note"] == ""



@pytest.mark.parametrize("age, started", [(1e11, 100 - 86400), (10**400, 100 - 86400), (float("inf"), 100), (float("nan"), 100), (-5, 100), (90, 10)])
def test_background_age_is_finite_and_at_most_a_day(age, started):
    """An age past a day (or none a clock can hold) would make a start no datetime can show."""
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working", tasks=[{"id": "a1", "title": "t", "age": age}]), now=100)
    assert r.background(100)[0]["started"] == started


@pytest.mark.parametrize(
    "done, total, kept",
    [(10**30, 10**30, (10000, 10000)), (5, 10**30, (5, 10000)), (10**30, 5, (None, None)), (10000, 10000, (10000, 10000))],
)
def test_background_progress_is_bounded(done, total, kept):
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working", tasks=[{"id": "a1", "title": "t", "done": done, "total": total}]), now=100)
    row = r.background(100)[0]
    assert (row["done"], row["total"]) == kept


def test_background_tool_calls_are_bounded():
    r = Registry()
    r.report(report("pc1", "a", 1, "A", "working", tasks=[{"id": "a1", "title": "t", "calls": 10**30}]), now=100)
    assert r.background(100)[0]["calls"] == 1_000_000


def test_a_long_task_list_is_read_only_so_far():
    """A report with thousands of tasks costs no more than its first TASK_LIMIT * 4; duplicates count once."""
    r = Registry()
    tasks = [{"id": "dup", "title": "first"}, {"id": "dup", "title": "second"}] + [{"id": f"t{i}", "title": "t"} for i in range(5000)]
    r.report(report("pc1", "a", 1, "A", "working", tasks=tasks), now=100)
    ids = {t["id"] for t in r.background(100)}
    # The first 32 entries are read: dup (twice) and t0..t29; the newest 8 of them stay.
    assert ids == {f"t{i}" for i in range(22, 30)}


if __name__ == "__main__":
    sys.exit(pytest.main([__file__]))
