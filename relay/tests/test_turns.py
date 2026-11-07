import threading

import pytest

from muse_relay.openai_api import MAX_CHARS
from muse_relay.turns import CONTINUE_NOTE, MSG_INTERNAL, STILL_WORKING, LinkError, TurnManager


class FakeLink:
    """run_turn blocks until release() unless auto; pieces go to on_text first."""

    def __init__(self, reply="こんにちは", pieces=(), auto=True, error=None):
        self.reply = reply
        self.pieces = pieces
        self.auto = auto
        self.error = error
        self.prompts = []
        self.started = threading.Event()
        self._go = threading.Event()

    def release(self):
        self._go.set()

    def run_turn(self, text, on_text):
        self.prompts.append(text)
        for p in self.pieces:
            on_text(p)
        self.started.set()
        if not self.auto:
            self._go.wait(5)
        if self.error:
            raise LinkError(self.error)
        return self.reply


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


def test_reply_within_wait_is_returned_whole():
    turns = TurnManager(FakeLink(reply="はい、送るよ。"), wait_s=2)
    assert turns.ask("送って") == "はい、送るよ。"


def test_link_error_message_is_the_reply():
    turns = TurnManager(FakeLink(error="Muse に届きませんでした (x)。"), wait_s=2)
    assert turns.ask("q") == "Muse に届きませんでした (x)。"


def test_slow_reply_returns_partial_then_held_reply():
    link = FakeLink(reply="途中まで。続き。", pieces=("途中まで。",), auto=False)
    turns = TurnManager(link, wait_s=1.0)
    assert turns.ask("q1") == "途中まで。\n" + CONTINUE_NOTE
    link.release()
    _wait_idle(turns)
    assert turns.ask("続き") == "途中まで。続き。"
    assert link.prompts == ["q1"]          # the held reply was returned instead of sending "続き"


def test_partial_is_the_latest_snapshot_not_a_concatenation():
    link = FakeLink(reply="全文", pieces=("あ", "あい", "あいう"), auto=False)
    turns = TurnManager(link, wait_s=1.0)
    assert turns.ask("q1") == "あいう\n" + CONTINUE_NOTE
    link.release()


def test_long_partial_is_clipped_so_the_note_survives():
    link = FakeLink(pieces=("あ" * 1000,), auto=False)
    turns = TurnManager(link, wait_s=1.0)
    out = turns.ask("q1")
    assert out.endswith(CONTINUE_NOTE) and len(out) <= MAX_CHARS
    link.release()


def test_slow_reply_without_partial_returns_note_only():
    link = FakeLink(auto=False)
    turns = TurnManager(link, wait_s=0.2)
    assert turns.ask("q1") == CONTINUE_NOTE
    link.release()


def test_request_while_running_says_still_working():
    link = FakeLink(auto=False)
    turns = TurnManager(link, wait_s=0.2)
    turns.ask("q1")
    assert turns.ask("q2") == STILL_WORKING
    assert link.prompts == ["q1"]
    link.release()


def test_held_reply_expires():
    clock = Clock()
    link = FakeLink(reply="古い返事", auto=False)
    turns = TurnManager(link, wait_s=0.2, hold_s=600, clock=clock)
    turns.ask("q1")
    link.release()
    _wait_idle(turns)
    clock.now += 601
    link.reply, link.auto = "新しい返事", True
    assert turns.ask("q2") == "新しい返事"
    assert link.prompts == ["q1", "q2"]


def test_held_reply_is_returned_once():
    link = FakeLink(reply="保持", auto=False)
    turns = TurnManager(link, wait_s=0.2)
    turns.ask("q1")
    link.release()
    _wait_idle(turns)
    assert turns.ask("q2") == "保持"
    link.reply, link.auto = "次", True
    assert turns.ask("q3") == "次"


class BrokenLink:
    def run_turn(self, text, on_text):
        raise RuntimeError("boom")


def test_unexpected_error_still_replies_and_leaves_manager_idle():
    turns = TurnManager(BrokenLink(), wait_s=2)
    assert turns.ask("q") == MSG_INTERNAL
    assert not turns.busy()
    assert turns.ask("q") == MSG_INTERNAL


def _wait_idle(turns):
    for _ in range(100):
        if not turns.busy():
            return
        threading.Event().wait(0.02)
    pytest.fail("turn did not finish")
