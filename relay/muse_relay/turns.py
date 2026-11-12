"""Turns between Even AI requests and Muse typed turns.

Even AI gives up after about 30 s, so a request waits at most WAIT_S. A turn
that takes longer keeps running; its reply is held and returned to the next
request that asks for it ("続き"), which is not sent to Muse. A reply longer
than a page is split and the rest held the same way: "続き" returns the next
page, "終わりまで" the rest at once, and any other prompt drops it and asks Muse. After the hold
expires, a paging word gets EXPIRED_NOTE instead of going to Muse, until the next new question.
"""
import threading
import time
from typing import Callable, Optional

from .openai_api import ALL_MAX_CHARS, BLANK_CHARS, MAX_CHARS, clip, split_page

WAIT_S = 25.0
HOLD_S = 10 * 60.0
STILL_WORKING = "まだ考え中です。少ししてからもう一度話しかけてください。"
CONTINUE_NOTE = "(まだ考え中です。「続き」と話しかけると続きを返します)"
PAGE_NOTE = "(続きは「続き」、残り全部は「終わりまで」と話しかけてください)"
EMPTY_NOTE = "(Muse の返事にテキストがありませんでした。リンクや画像は Gadget に届きません)"
MSG_INTERNAL = "Muse に届きませんでした (内部エラー)。"
EXPIRED_NOTE = "(続きの保持は期限切れです。もう一度質問してください)"

# Words that ask for held pages, and what may follow them; edit here.
PAGE_ALL_WORDS = ("終わりまで", "おわりまで", "最後まで", "さいごまで", "全部", "ぜんぶ", "全文",
                  "続きを全部", "続き全部", "残り全部", "残り")
PAGE_CONTINUE_WORDS = ("続き", "つづき", "続けて", "つづけて", "次", "つぎ", "もっと")
PAGE_KEY_SUFFIXES = ("", "を", "を見せて", "をみせて", "を教えて", "をおしえて", "をお願い", "をおねがい", "をください",
                     "お願い", "おねがい", "見せて", "みせて", "して", "のページ", "ページ", "読んで", "よんで",
                     "を読んで", "をよんで")
PAGE_KEY_IGNORED = frozenset(BLANK_CHARS + "。、．，！？!?.,「」『』…ー〜~")


def _key_text(text: str) -> str:
    return "".join(c for c in text if c not in PAGE_KEY_IGNORED)


# The suffixes without the ignored characters too ("ページ" has a "ー").
_SUFFIX_KEYS = frozenset(_key_text(s) for s in PAGE_KEY_SUFFIXES)


def _is_word_with_suffix(key: str, words: tuple[str, ...]) -> bool:
    return any(key.startswith(w) and key[len(w):] in _SUFFIX_KEYS for w in words)


def page_key(prompt: str) -> Optional[str]:
    """"all", "continue" or None (a new question). Spaces and punctuation are ignored; what is left
    must be one of the words followed by one of PAGE_KEY_SUFFIXES. ALL words are tried first."""
    key = _key_text(prompt)
    if _is_word_with_suffix(key, PAGE_ALL_WORDS):
        return "all"
    if _is_word_with_suffix(key, PAGE_CONTINUE_WORDS):
        return "continue"
    return None


def _room(limit: int, note: str) -> int:
    """Characters left for the text when "\n" + note must also fit in limit; at least 1."""
    return max(limit - len(note) - 1, 1)


class LinkError(Exception):
    """A failed turn; str() is the message to show on the glasses."""


class _Turn:
    def __init__(self) -> None:
        self.partial_text = ""     # the latest snapshot of the reply so far
        self.reply = ""
        self.error: Optional[str] = None
        self.finished = False
        self.detached = False      # the request stopped waiting: hold the result
        self.done = threading.Event()

    def result(self) -> str:
        """A reply without text or widgets (Muse sent only a link, say) gets a note."""
        if self.error is not None:
            return self.error
        return self.reply if self.reply.strip(BLANK_CHARS) else EMPTY_NOTE

    def partial(self) -> str:
        return self.partial_text


class TurnManager:
    def __init__(self, link, wait_s: float = WAIT_S, hold_s: float = HOLD_S,
                 clock: Callable[[], float] = time.monotonic, max_chars: int = MAX_CHARS,
                 all_max_chars: int = ALL_MAX_CHARS) -> None:
        self._link = link
        self._max_chars = max_chars
        self._all_max_chars = all_max_chars
        self._wait_s = wait_s
        self._hold_s = hold_s
        self._clock = clock
        self._lock = threading.Lock()
        self._running: Optional[_Turn] = None
        self._held: Optional[tuple[str, float]] = None
        self._expired = False      # the held reply expired; cleared by the next new turn

    def busy(self) -> bool:
        with self._lock:
            return self._running is not None

    def ask(self, text: str) -> str:
        with self._lock:
            held = self._take_held(text)
            if held is not None:
                return held
            if self._running is not None:
                return STILL_WORKING
            turn = _Turn()
            self._running = turn
            self._expired = False
        threading.Thread(target=self._run, args=(turn, text), daemon=True).start()
        turn.done.wait(self._wait_s)
        with self._lock:
            if turn.finished:
                return self._page(turn.result())
            turn.detached = True
            partial = turn.partial()
        if not partial:
            return CONTINUE_NOTE
        return f"{clip(partial, _room(self._max_chars, CONTINUE_NOTE))}\n{CONTINUE_NOTE}"

    def _run(self, turn: _Turn, text: str) -> None:
        try:
            turn.reply = self._link.run_turn(text, lambda partial: setattr(turn, "partial_text", partial))
        except LinkError as e:
            turn.error = str(e)
        except Exception:
            turn.error = MSG_INTERNAL
        finally:
            with self._lock:
                self._running = None
                turn.finished = True
                if turn.detached:
                    self._held = (turn.result(), self._clock())
            turn.done.set()

    def _page(self, text: str, limit: Optional[int] = None) -> str:
        """The first page (at most limit characters) of a finished reply; the rest is held.
        Call with the lock held."""
        limit = self._max_chars if limit is None else limit
        if len(text) <= limit:
            return text
        page, rest = split_page(text, _room(limit, PAGE_NOTE))
        if not rest:
            return page
        self._held = (rest, self._clock())
        return f"{page}\n{PAGE_NOTE}"

    def _expire_held(self) -> None:
        if self._held is not None and self._clock() - self._held[1] > self._hold_s:
            self._held = None
            self._expired = True

    def _take_held(self, prompt: str) -> Optional[str]:
        """The held reply's next page if prompt asks for one, or EXPIRED_NOTE if it expired; any
        other prompt drops it. Call with the lock held."""
        self._expire_held()
        key = page_key(prompt)
        held, self._held = self._held, None
        if key is None:
            return None
        if self._expired:
            return EXPIRED_NOTE
        if held is None:
            return None
        return self._page(held[0], self._all_max_chars if key == "all" else self._max_chars)
