"""Typed turns on a board over USB, through the Muse Gadget SDK's tools/muse/chat.py."""
import importlib
import sys
import threading
from pathlib import Path
from typing import Callable, Optional

from .turns import LinkError

BUSY_MARK = "BUSY WITH A VOICE TURN"
MSG_BUSY = "StickS3 で音声の会話中です。終わってからもう一度どうぞ。"
REASON_MAX = 60
MUSE_ERROR_MARK = "Muse: "      # the board answered with an error: the cable is fine, keep the port
MSG_INTERNAL = "Muse に届きませんでした (内部エラー)。"


def sdk_tools_dir() -> Path:
    return Path(__file__).resolve().parents[2] / "hub" / "third_party" / "muse-gadget-sdk" / "esp32" / "tools" / "muse"


def _load_chat():
    path = str(sdk_tools_dir())
    if path not in sys.path:
        sys.path.insert(0, path)     # chat.py imports its sibling ports.py
    return importlib.import_module("chat")


def _reason(msg: str) -> str:
    first = msg.strip().splitlines()[0] if msg.strip() else "unknown"
    return first if len(first) <= REASON_MAX else first[:REASON_MAX - 1] + "…"


class BoardLink:
    """One board, one turn at a time. The port stays open: opening it resets the StickS3."""

    def __init__(self, port: str, chat_module=None) -> None:
        self._chat = chat_module or _load_chat()
        self._port = port
        self._lock = threading.Lock()
        self._board = None

    def open(self) -> Optional[dict]:
        """Opens the board and returns its status (None if it doesn't answer)."""
        with self._lock:
            return self._open().status()

    def run_turn(self, text: str, on_text: Callable[[str], None]) -> str:
        with self._lock:
            try:
                board = self._open()
                reply = self._chat.Reply()

                def progress(frame: dict) -> None:
                    reply.feed(frame)
                    if frame.get("type") in ("text", "final"):
                        on_text(reply.text)       # the whole partial text so far

                return board.chat(text, progress).text
            except (self._chat.BoardError, OSError) as e:
                msg = str(e)
                if BUSY_MARK in msg:
                    raise LinkError(MSG_BUSY) from e
                if not msg.startswith(MUSE_ERROR_MARK):
                    self._close()
                raise LinkError(f"Muse に届きませんでした ({_reason(msg)})。") from e
            except Exception as e:
                self._close()
                raise LinkError(MSG_INTERNAL) from e

    def _open(self):
        if self._board is None:
            self._board = self._chat.Board(self._port)
        return self._board

    def _close(self) -> None:
        if self._board is not None:
            try:
                self._board.close()
            except OSError:
                pass
            self._board = None
