import threading

import pytest

from muse_relay.openai_api import MAX_CHARS, split_page
from muse_relay.turns import (CONTINUE_NOTE, EMPTY_NOTE, MSG_INTERNAL, PAGE_NOTE, STILL_WORKING, LinkError,
                              TurnManager, page_key)

PAGE_CHARS = 300
PAGE_ROOM = PAGE_CHARS - len(PAGE_NOTE) - 1


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
    link = FakeLink(pieces=("あ" * (MAX_CHARS + 1000),), auto=False)
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
    assert turns.ask("続き") == "保持"
    link.reply, link.auto = "次", True
    assert turns.ask("続き") == "次"


class BrokenLink:
    def run_turn(self, text, on_text):
        raise RuntimeError("boom")


def test_unexpected_error_still_replies_and_leaves_manager_idle():
    turns = TurnManager(BrokenLink(), wait_s=2)
    assert turns.ask("q") == MSG_INTERNAL
    assert not turns.busy()
    assert turns.ask("q") == MSG_INTERNAL


def _unpage(page):
    suffix = "\n" + PAGE_NOTE
    return page[:-len(suffix)] if page.endswith(suffix) else page


def test_long_reply_is_paged():
    link = FakeLink(reply="あ" * 700)
    turns = TurnManager(link, wait_s=2, max_chars=PAGE_CHARS)
    pages = [turns.ask("q"), turns.ask("続き"), turns.ask("続き")]
    assert all(len(p) <= PAGE_CHARS for p in pages)
    assert pages[0].endswith("\n" + PAGE_NOTE) and pages[1].endswith("\n" + PAGE_NOTE)
    assert PAGE_NOTE not in pages[2]
    assert "".join(_unpage(p) for p in pages) == "あ" * 700
    assert link.prompts == ["q"]
    link.reply = "次"
    assert turns.ask("次") == "次"
    assert link.prompts == ["q", "次"]


def test_page_split_prefers_a_sentence_end():
    turns = TurnManager(FakeLink(reply="あ" * 250 + "。" + "い" * 200), wait_s=2, max_chars=PAGE_CHARS)
    assert turns.ask("q") == "あ" * 250 + "。\n" + PAGE_NOTE
    assert turns.ask("続き") == "い" * 200


def test_page_split_ignores_an_early_sentence_end():
    turns = TurnManager(FakeLink(reply="あ" * 100 + "。" + "い" * 300), wait_s=2, max_chars=PAGE_CHARS)
    out = turns.ask("q")
    assert out.endswith("\n" + PAGE_NOTE) and len(out) == PAGE_CHARS


def test_split_page_strips_newlines_at_the_cut():
    assert split_page("あ" * 250 + "\n\n" + "い" * 200, PAGE_ROOM) == ("あ" * 250, "い" * 200)
    assert split_page("abcd！efgh", 5) == ("abcd！", "efgh")
    assert split_page("あいう", 5) == ("あいう", "")


def test_held_page_expires():
    clock = Clock()
    link = FakeLink(reply="あ" * 700)
    turns = TurnManager(link, wait_s=2, hold_s=600, clock=clock, max_chars=PAGE_CHARS)
    assert turns.ask("q1").endswith(PAGE_NOTE)
    clock.now += 601
    link.reply = "新しい返事"
    assert turns.ask("q2") == "新しい返事"
    assert link.prompts == ["q1", "q2"]


def test_timeout_held_long_reply_is_paged():
    link = FakeLink(reply="あ" * 500, auto=False)
    turns = TurnManager(link, wait_s=0.2, max_chars=PAGE_CHARS)
    assert turns.ask("q1") == CONTINUE_NOTE
    link.release()
    _wait_idle(turns)
    assert turns.ask("続き") == "あ" * PAGE_ROOM + "\n" + PAGE_NOTE
    assert turns.ask("続き") == "あ" * (500 - PAGE_ROOM)
    assert link.prompts == ["q1"]


@pytest.mark.parametrize("reply", ["", "\n\n"])
def test_empty_reply_gives_the_note(reply):
    assert TurnManager(FakeLink(reply=reply), wait_s=2).ask("q") == EMPTY_NOTE


def test_empty_held_reply_gives_the_note():
    link = FakeLink(reply="", auto=False)
    turns = TurnManager(link, wait_s=0.2)
    turns.ask("q1")
    link.release()
    _wait_idle(turns)
    assert turns.ask("続き") == EMPTY_NOTE


KEY_ALL_MAX = 2000


def _first_page(all_max_chars=KEY_ALL_MAX):
    link = FakeLink(reply="あ" * 1500)
    turns = TurnManager(link, wait_s=2, max_chars=PAGE_CHARS, all_max_chars=all_max_chars)
    assert turns.ask("q").endswith("\n" + PAGE_NOTE)
    return link, turns


@pytest.mark.parametrize("prompt", ["続き", "続きを見せて。", " つ づ き ！", "「次」", "もっと…"])
def test_continue_words_give_the_next_page(prompt):
    link, turns = _first_page()
    assert turns.ask(prompt) == "あ" * PAGE_ROOM + "\n" + PAGE_NOTE
    assert link.prompts == ["q"]


@pytest.mark.parametrize("prompt", ["終わりまで", "最後まで。", "「全部」！", "全文", "続きを全部"])
def test_all_words_give_the_whole_rest(prompt):
    link, turns = _first_page()
    assert turns.ask(prompt) == "あ" * (1500 - PAGE_ROOM)
    link.reply = "次の返事"
    assert turns.ask("続き") == "次の返事"       # nothing is held any more
    assert link.prompts == ["q", "続き"]


def test_all_pages_a_rest_longer_than_all_max():
    link, turns = _first_page(all_max_chars=500)
    assert turns.ask("全部") == "あ" * (500 - len(PAGE_NOTE) - 1) + "\n" + PAGE_NOTE
    out = turns.ask("続き")
    assert out.endswith("\n" + PAGE_NOTE) and len(out) <= PAGE_CHARS
    assert link.prompts == ["q"]


@pytest.mark.parametrize("prompt", ["明日の天気は", "次の東京の天気と明日の大阪の天気を教えてください"])
def test_new_question_drops_the_held_pages(prompt):
    link, turns = _first_page()
    link.reply = "新しい返事"
    assert turns.ask(prompt) == "新しい返事"
    assert link.prompts == ["q", prompt]
    assert turns.ask("続き") == "新しい返事"     # the old pages are gone
    assert link.prompts == ["q", prompt, "続き"]


@pytest.mark.parametrize("prompt, key", [
    ("続き", "continue"), ("続きを見せて。", "continue"), ("\u3000つづき、！", "continue"), ("もっと", "continue"),
    ("「終わり、まで」…", "all"), ("続きを全部", "all"), ("さいごまで", "all"),
    ("次の東京の天気と明日の大阪の天気を教えてください", None), ("天気は", None), ("", None),
    ("続きの話はまた今度にしましょうね", None),
])
def test_page_key(prompt, key):
    assert page_key(prompt) == key


def _wait_idle(turns):
    for _ in range(100):
        if not turns.busy():
            return
        threading.Event().wait(0.02)
    pytest.fail("turn did not finish")
