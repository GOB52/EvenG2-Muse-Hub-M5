"""Turns between Even AI requests and Muse typed turns.

Even AI gives up after about 30 s, so a request waits at most WAIT_S. A turn
that takes longer keeps running; its reply is held and returned to the next
request, which is not sent to Muse.
"""
import threading
import time
from typing import Callable, Optional

from .openai_api import MAX_CHARS, clip

WAIT_S = 25.0
HOLD_S = 10 * 60.0
STILL_WORKING = "まだ考え中です。少ししてからもう一度話しかけてください。"
CONTINUE_NOTE = "(まだ考え中です。もう一度話しかけると続きを返します)"
MSG_INTERNAL = "Muse に届きませんでした (内部エラー)。"


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
        return self.error if self.error is not None else self.reply

    def partial(self) -> str:
        return self.partial_text


class TurnManager:
    def __init__(self, link, wait_s: float = WAIT_S, hold_s: float = HOLD_S,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._link = link
        self._wait_s = wait_s
        self._hold_s = hold_s
        self._clock = clock
        self._lock = threading.Lock()
        self._running: Optional[_Turn] = None
        self._held: Optional[tuple[str, float]] = None

    def busy(self) -> bool:
        with self._lock:
            return self._running is not None

    def ask(self, text: str) -> str:
        with self._lock:
            held = self._take_held()
            if held is not None:
                return held
            if self._running is not None:
                return STILL_WORKING
            turn = _Turn()
            self._running = turn
        threading.Thread(target=self._run, args=(turn, text), daemon=True).start()
        turn.done.wait(self._wait_s)
        with self._lock:
            if turn.finished:
                return turn.result()
            turn.detached = True
            partial = turn.partial()
        if not partial:
            return CONTINUE_NOTE
        return f"{clip(partial, MAX_CHARS - len(CONTINUE_NOTE) - 1)}\n{CONTINUE_NOTE}"

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

    def _take_held(self) -> Optional[str]:
        """Call with the lock held."""
        if self._held is None:
            return None
        text, at = self._held
        self._held = None
        if self._clock() - at > self._hold_s:
            return None
        return text
