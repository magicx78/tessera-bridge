"""The registry: every Claude session the bridges report, its questions, and the account's limits.

Plain Python, no Home Assistant in it, so the rules of several sessions, several displays and the race between the
PC and a display are unit-tested on their own (tests/test_registry.py). The integration wraps it: the HTTP API feeds
`report`, `claim` and `end`; the display's services call `overview`, `question`, `hint` and `answer`; the sensors
read `snapshot`.

Identity is never a text. A session is `<instance>.<session id>` (the mod's installation and the engine's session
id); a question is that key plus the call's tool_use_id, with a nonce per registration; parts and options carry the
ids the mod gave them (`p0`, `o1`). The display works with short handles the registry hands out (`s3`, `q7`): a handle
names one registration and is never reused, so an answer that arrives late, twice or from before a restart finds no
open question to land on.
"""
from __future__ import annotations

from dataclasses import dataclass
import secrets
import string
from typing import Any, Callable

# A session that did not report for this long is "lost" (its bridge, PC or network is away) ...
LOST_AFTER = 45.0
# ... and gone from the list after this long without a word.
FORGET_AFTER = 10 * 60.0
# An ended session stays in the list this long, as "ended".
KEEP_ENDED = 60.0
# How many sessions the sensor's lists carry (the display pages through them).
LIST_LIMIT = 12
# The longest name in the sensor's lists.
NAME_LIMIT = 24

STATE_ORDER = {"asking": 0, "permission": 1, "working": 2, "idle": 3, "lost": 4, "ended": 5}
# The states in which a session does something or waits for the person: what the activity tile counts.
ACTIVE_STATES = ("working", "asking", "permission")
STATE_CODES = {"asking": "ask", "permission": "perm", "working": "work", "idle": "idle", "lost": "lost", "ended": "end"}


@dataclass
class Session:
    key: str
    handle: str
    instance: str
    session_id: str
    name: str = ""
    instance_name: str = ""
    model: str = ""
    cwd: str = ""
    state: str = "idle"
    detail: str = ""
    context: dict[str, Any] | None = None
    seq: int = 0
    last_seen: float = 0.0
    ended: bool = False
    ended_at: float = 0.0
    # The permission hint the sound announced last, so it is said once.
    announced_hint: str = ""
    # Since when the session is active without a break (0: it is not), and when it was active last (0: never).
    active_since: float = 0.0
    last_active: float = 0.0

    def shown_state(self, now: float) -> str:
        if self.ended:
            return "ended"
        if now - self.last_seen > LOST_AFTER:
            return "lost"
        return self.state


@dataclass
class Question:
    key: str  # <session key>|<tool_use_id>
    handle: str
    session_key: str
    tool_use_id: str
    nonce: str
    parts: list[dict[str, Any]]
    rev: int
    created: float
    status: str = "open"  # open | answered | cancelled
    by: str = ""  # display | pc
    answer: dict[str, list[str]] | None = None
    answered_by_display: str = ""  # which display, as it named itself
    announced: bool = False

    def status_reply(self) -> dict[str, Any]:
        reply: dict[str, Any] = {"status": self.status, "nonce": self.nonce, "handle": self.handle}
        if self.by:
            reply["by"] = self.by
        if self.answer is not None:
            reply["answer"] = self.answer
        return reply


# What the Tessera app hands a screen of a text in an answer: 48 bytes. A question travels as lines under that.
LINE_BYTES = 46
LINE_LIMIT = 6
DESCRIPTION_LIMIT = 40


def wrap_lines(text: str, max_bytes: int = LINE_BYTES, max_lines: int = LINE_LIMIT) -> list[str]:
    """Breaks a text into lines of at most `max_bytes` of UTF-8 at the spaces, a long word in the middle; at most
    `max_lines`, the last one ending in an ellipsis when the text went on."""
    words = text.split()
    lines: list[str] = []
    line = ""
    for word in words:
        while len(word.encode("utf-8")) > max_bytes:
            cut = word
            while len(cut.encode("utf-8")) > max_bytes - 1:
                cut = cut[:-1]
            if line:
                lines.append(line)
                line = ""
            lines.append(cut)
            word = word[len(cut):]
        candidate = f"{line} {word}" if line else word
        if len(candidate.encode("utf-8")) <= max_bytes:
            line = candidate
        else:
            lines.append(line)
            line = word
    if line:
        lines.append(line)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        last = lines[-1]
        while len((last + "…").encode("utf-8")) > max_bytes:
            last = last[:-1]
        lines[-1] = last.rstrip() + "…"
    return lines


def _text(value: Any, limit: int | None = None) -> str:
    text = value if isinstance(value, str) else ""
    if limit is not None and len(text) > limit:
        text = text[: limit - 1] + "…"
    return text


def _parts_of(raw: Any) -> list[dict[str, Any]]:
    parts: list[dict[str, Any]] = []
    if not isinstance(raw, list):
        return parts
    for item in raw:
        if not isinstance(item, dict) or not isinstance(item.get("id"), str):
            continue
        options = []
        for option in item.get("options") or []:
            if isinstance(option, dict) and isinstance(option.get("id"), str):
                options.append(
                    {
                        "id": option["id"],
                        "label": _text(option.get("label")),
                        "description": _text(option.get("description"), DESCRIPTION_LIMIT),
                    }
                )
        parts.append(
            {
                "id": item["id"],
                "header": _text(item.get("header")),
                "text": _text(item.get("text")),
                "lines": wrap_lines(_text(item.get("text"))),
                "multi": item.get("multi") is True,
                "options": options,
            }
        )
    return parts


class Registry:
    """Sessions, questions and limits, and the one place that decides who answered a question first."""

    def __init__(self) -> None:
        self.sessions: dict[str, Session] = {}
        self.questions: dict[str, Question] = {}
        # The account's limits as each instance (computer) reported them last: (when, limits).
        self.limits: dict[str, tuple[float, list[dict[str, Any]]]] = {}
        # Handles carry a token of this registry's life, so a handle from before a restart fits nothing after it.
        self._boot = "".join(secrets.choice(string.ascii_lowercase) for _ in range(2))
        self._handles = 0
        self.rev = 0
        self.event_rev = 0
        self.event = ""
        self._listeners: list[Callable[[], None]] = []
        # What happened since the last `take_events`: new questions and new hints, for sounds and HA events.
        self._pending_events: list[dict[str, Any]] = []
        # The last activity of a session that is gone from the list, so "last active an hour ago" outlives it.
        self._last_active = 0.0

    # ---- Listening ----

    def listen(self, callback: Callable[[], None]) -> Callable[[], None]:
        self._listeners.append(callback)

        def unlisten() -> None:
            self._listeners.remove(callback)

        return unlisten

    def _changed(self) -> None:
        self.rev += 1
        for callback in list(self._listeners):
            callback()

    def _event(self, kind: str, **data: Any) -> None:
        self.event_rev += 1
        self.event = f"{kind}:{data.get('handle', '')}"
        self._pending_events.append({"kind": kind, **data})

    def take_events(self) -> list[dict[str, Any]]:
        events, self._pending_events = self._pending_events, []
        return events

    def _handle(self, prefix: str) -> str:
        self._handles += 1
        return f"{prefix}{self._boot}{self._handles}"

    # ---- What the bridge sends ----

    def report(self, body: dict[str, Any], now: float) -> dict[str, Any]:
        """A session's whole state. Returns the session's handle and every reported question's status."""
        instance = _text(body.get("instance"))
        session_id = _text(body.get("session"))
        if not instance or not session_id:
            return {"ok": False, "error": "no session"}
        key = f"{instance}.{session_id}"
        seq = body.get("seq") if isinstance(body.get("seq"), int) else 0
        session = self.sessions.get(key)
        if session is None:
            session = Session(key=key, handle=self._handle("s"), instance=instance, session_id=session_id)
            self.sessions[key] = session
        elif seq and seq < session.seq:
            # A report that was on its way longer than a newer one: the newer one stands, but the questions it
            # asks about get their answer, as any poll does.
            return {"ok": True, "handle": session.handle, "stale": True, "questions": self._statuses(session, body)}
        session.seq = seq
        session.last_seen = now
        session.ended = False
        session.name = _text(body.get("name"), 64) or session.name or session_id[:8]
        session.instance_name = _text(body.get("instance_name"), 32)
        session.model = _text(body.get("model"), 32)
        session.cwd = _text(body.get("cwd"), 256)
        state = _text(body.get("state"))
        session.state = state if state in STATE_ORDER else "idle"
        session.detail = _text(body.get("detail"), 200)
        self._track_activity(session, now)
        context = body.get("context")
        session.context = context if isinstance(context, dict) else None
        limits = body.get("limits")
        if isinstance(limits, list):
            self._take_limits(instance, limits, body.get("limits_age"), now)
        if session.state == "permission" and session.detail and session.announced_hint != session.detail:
            session.announced_hint = session.detail
            self._event("hint", handle=session.handle, session=session.name, detail=session.detail)
        if session.state != "permission":
            session.announced_hint = ""
        self._sync_questions(session, body, now)
        self._changed()
        return {"ok": True, "handle": session.handle, "questions": self._statuses(session, body)}

    def _sync_questions(self, session: Session, body: dict[str, Any], now: float) -> None:
        reported = body.get("questions")
        reported = reported if isinstance(reported, list) else []
        seen: set[str] = set()
        for item in reported:
            if not isinstance(item, dict) or not isinstance(item.get("id"), str):
                continue
            tool_use_id = item["id"]
            nonce = _text(item.get("nonce"))
            key = f"{session.key}|{tool_use_id}"
            seen.add(key)
            question = self.questions.get(key)
            if question is not None and question.nonce == nonce:
                continue  # known, in whatever state it is: the reply says
            if question is not None:
                # Another registration of the same call (the mod reloaded): the old one is over.
                self._cancel(question)
            parts = _parts_of(item.get("parts"))
            if not parts:
                continue
            question = Question(
                key=key,
                handle=self._handle("q"),
                session_key=session.key,
                tool_use_id=tool_use_id,
                nonce=nonce,
                parts=parts,
                rev=self.rev + 1,
                created=now,
            )
            self.questions[key] = question
            question.announced = True
            self._event("question", handle=question.handle, session=session.name, text=parts[0]["text"])
        # A question of this session the bridge no longer lists is over: answered on the PC, aborted, or the mod
        # reloaded. Only this session's.
        for question in list(self.questions.values()):
            if question.session_key == session.key and question.key not in seen and question.status == "open":
                self._cancel(question)

    def _statuses(self, session: Session, body: dict[str, Any]) -> dict[str, Any]:
        statuses: dict[str, Any] = {}
        reported = body.get("questions")
        for item in reported if isinstance(reported, list) else []:
            if isinstance(item, dict) and isinstance(item.get("id"), str):
                question = self.questions.get(f"{session.key}|{item['id']}")
                statuses[item["id"]] = question.status_reply() if question else {"status": "unknown"}
        return statuses

    def _cancel(self, question: Question) -> None:
        if question.status == "open":
            question.status = "cancelled"

    def claim(self, body: dict[str, Any]) -> dict[str, Any]:
        """The PC answered (or dismissed) a question. Accepted unless a display's answer was first."""
        key = f"{_text(body.get('instance'))}.{_text(body.get('session'))}|{_text(body.get('id'))}"
        question = self.questions.get(key)
        outcome = "cancelled" if body.get("outcome") == "cancelled" else "answered"
        if question is None or question.nonce != _text(body.get("nonce")):
            return {"accepted": True, "status": "unknown"}
        if question.status == "open":
            question.status = outcome
            question.by = "pc"
            self._changed()
            return {"accepted": True, "status": question.status, "by": "pc"}
        if question.status == "answered" and question.by == "display":
            return {"accepted": False, "status": "answered", "by": "display", "answer": question.answer}
        return {"accepted": True, "status": question.status, "by": question.by}

    def end(self, body: dict[str, Any], now: float) -> dict[str, Any]:
        key = f"{_text(body.get('instance'))}.{_text(body.get('session'))}"
        session = self.sessions.get(key)
        if session is None:
            return {"ok": True}
        session.ended = True
        session.ended_at = now
        session.state = "idle"
        self._track_activity(session, now)
        session.detail = ""
        for question in self.questions.values():
            if question.session_key == key:
                self._cancel(question)
        self._changed()
        return {"ok": True}

    @staticmethod
    def _track_activity(session: Session, now: float) -> None:
        """Idle to active starts the clock, active to idle stops it; working, asking and waiting in a row are one
        activity."""
        if session.state in ACTIVE_STATES:
            if not session.active_since:
                session.active_since = now
            session.last_active = now
        elif session.active_since:
            session.active_since = 0.0
            session.last_active = now

    # ---- Time ----

    def prune(self, now: float) -> bool:
        """Forgets sessions long gone and their questions; returns whether anything changed."""
        changed = False
        for key, session in list(self.sessions.items()):
            gone = (session.ended and now - session.ended_at > KEEP_ENDED) or (
                not session.ended and now - session.last_seen > FORGET_AFTER
            )
            if gone:
                self._last_active = max(self._last_active, session.last_active)
                del self.sessions[key]
                for qkey, question in list(self.questions.items()):
                    if question.session_key == key:
                        del self.questions[qkey]
                changed = True
        # Settled questions of living sessions go after a while too.
        for qkey, question in list(self.questions.items()):
            if question.status != "open" and now - question.created > FORGET_AFTER:
                del self.questions[qkey]
        if changed:
            self._changed()
        return changed

    # ---- What the display asks ----

    def _session_of(self, question: Question) -> Session | None:
        return self.sessions.get(question.session_key)

    def _ordered(self, now: float) -> list[Session]:
        return sorted(
            self.sessions.values(),
            key=lambda s: (STATE_ORDER.get(s.shown_state(now), 9), s.name.lower(), s.handle),
        )

    def open_questions(self, session: Session) -> list[Question]:
        return sorted(
            (q for q in self.questions.values() if q.session_key == session.key and q.status == "open"),
            key=lambda q: q.created,
        )

    def overview(self, now: float) -> dict[str, Any]:
        """Every session for the display's list, and the limits; the display pages through it."""
        sessions = []
        for session in self._ordered(now):
            open_questions = self.open_questions(session)
            sessions.append(
                {
                    "h": session.handle,
                    "name": _text(session.name, NAME_LIMIT),
                    "pc": _text(session.instance_name, NAME_LIMIT),
                    "state": STATE_CODES.get(session.shown_state(now), "idle"),
                    "ctx": self._context_percent(session),
                    "asks": len(open_questions),
                    "q": open_questions[0].handle if open_questions else "",
                    "detail": _text(session.detail, 96),
                }
            )
        return {"sessions": sessions, "limits": self.limits_summary(now)}

    def question(self, handle: str) -> dict[str, Any]:
        for question in self.questions.values():
            if question.handle == handle:
                session = self._session_of(question)
                return {
                    "h": question.handle,
                    "rev": question.rev,
                    "status": question.status,
                    "session": session.handle if session else "",
                    "name": _text(session.name, NAME_LIMIT) if session else "",
                    "parts": question.parts,
                }
        return {"h": handle, "status": "unknown", "parts": []}

    def hint(self, handle: str, now: float) -> dict[str, Any]:
        for session in self.sessions.values():
            if session.handle == handle:
                return {
                    "h": handle,
                    "name": _text(session.name, NAME_LIMIT),
                    "state": STATE_CODES.get(session.shown_state(now), "idle"),
                    "detail": session.detail,
                }
        return {"h": handle, "state": "unknown", "detail": ""}

    def answer(self, handle: str, rev: Any, answers: Any, display: str = "") -> dict[str, Any]:
        """A display's answer. Exactly one answer takes effect, whoever and however many try."""
        question = next((q for q in self.questions.values() if q.handle == handle), None)
        if question is None:
            return {"accepted": False, "reason": "unknown"}
        if question.status != "open":
            return {"accepted": False, "reason": question.status, "by": question.by}
        if rev is not None and rev != question.rev:
            return {"accepted": False, "reason": "stale"}
        session = self._session_of(question)
        if session is None or session.ended:
            return {"accepted": False, "reason": "ended"}
        cleaned = self._check_answer(question, answers)
        if cleaned is None:
            return {"accepted": False, "reason": "bad_answer"}
        question.status = "answered"
        question.by = "display"
        question.answer = cleaned
        question.answered_by_display = _text(display, 32)
        self._changed()
        return {"accepted": True}

    @staticmethod
    def _check_answer(question: Question, answers: Any) -> dict[str, list[str]] | None:
        if not isinstance(answers, dict):
            return None
        cleaned: dict[str, list[str]] = {}
        for part in question.parts:
            chosen = answers.get(part["id"])
            if isinstance(chosen, str):
                chosen = [chosen]
            if not isinstance(chosen, list) or not chosen:
                return None
            if not part["multi"] and len(chosen) != 1:
                return None
            ids = {o["id"] for o in part["options"]}
            if any(not isinstance(c, str) or c not in ids for c in chosen):
                return None
            cleaned[part["id"]] = list(dict.fromkeys(chosen))
        return cleaned

    # ---- What the sensors show ----

    @staticmethod
    def _context_percent(session: Session) -> int:
        context = session.context or {}
        percent = context.get("percent")
        if isinstance(percent, (int, float)):
            return max(0, min(100, round(percent)))
        return -1

    def _take_limits(self, instance: str, limits: list, age: Any, now: float) -> None:
        """A session's reading of the account's limits. Every session repeats its own reading on every heartbeat,
        an idle one for hours: the reading measured last stands (`limits_age`: seconds since the session measured
        it), and a session without a reading (an empty list) takes nothing away."""
        clean = [l for l in limits if isinstance(l, dict) and isinstance(l.get("kind"), str)]
        if not clean:
            return
        measured = now - age if isinstance(age, (int, float)) and not isinstance(age, bool) and age >= 0 else now
        known = self.limits.get(instance)
        if known is None or measured >= known[0]:
            self.limits[instance] = (measured, clean)

    def limits_summary(self, now: float) -> dict[str, Any]:
        """The account's limits from the instance that reported last: never summed over sessions."""
        if not self.limits:
            return {}
        when, limits = max(self.limits.values(), key=lambda item: item[0])
        summary: dict[str, Any] = {"at": when, "kinds": {}}
        for limit in limits:
            kind = limit["kind"]
            percent = limit.get("percent")
            summary["kinds"][kind] = {
                "percent": round(percent) if isinstance(percent, (int, float)) else None,
                "resets_at": limit.get("resets_at") if isinstance(limit.get("resets_at"), str) else None,
            }
        return summary

    def live_sessions(self, now: float) -> list[Session]:
        return [s for s in self.sessions.values() if s.shown_state(now) not in ("ended",)]

    def snapshot(self, now: float) -> dict[str, Any]:
        """The sessions sensor's attributes: parallel lists, texts and numbers only, bounded for the screen. The times
        (`active_since`, `last_active`) are on the registry's clock, `now`'s; 0 for none."""
        ordered = [s for s in self._ordered(now) if not s.ended][:LIST_LIMIT]
        handles, names, states, ctx, asks, first = [], [], [], [], [], []
        open_total = 0
        for session in ordered:
            open_questions = self.open_questions(session)
            open_total += len(open_questions)
            handles.append(session.handle)
            names.append(_text(session.name, NAME_LIMIT))
            states.append(STATE_CODES.get(session.shown_state(now), "idle"))
            ctx.append(self._context_percent(session))
            asks.append(len(open_questions))
            first.append(open_questions[0].handle if open_questions else "")
        open_total = sum(1 for q in self.questions.values() if q.status == "open" and q.session_key in self.sessions)
        # A lost session is not active, whatever it said last: its start does not count, its last word does.
        active = [s for s in self.sessions.values() if s.shown_state(now) in ACTIVE_STATES and s.active_since]
        last_active = max([self._last_active, *(s.last_active for s in self.sessions.values())])
        return {
            "rev": self.rev,
            "event_rev": self.event_rev,
            "event": self.event,
            "open": open_total,
            "h": handles,
            "names": names,
            "states": states,
            "ctx": ctx,
            "asks": asks,
            "q": first,
            "count": len(self.live_sessions(now)),
            "active": len(active),
            "active_since": min((s.active_since for s in active), default=0.0),
            "last_active": last_active,
        }
