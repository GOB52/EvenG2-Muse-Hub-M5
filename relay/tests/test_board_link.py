import types

import pytest

from muse_relay.board_link import MSG_BUSY, BoardLink, sdk_tools_dir
from muse_relay.turns import LinkError


def make_chat(frames=(), reply_text="返事", error=None):
    """A stand-in for the SDK's chat module."""
    mod = types.SimpleNamespace()

    class BoardError(Exception):
        pass

    class Board:
        opened = []

        def __init__(self, port):
            Board.opened.append(port)
            self.closed = False

        def status(self, timeout=3.0):
            return {"board": "M5Stack StickS3"}

        def chat(self, text, progress=None):
            for f in frames:
                progress(f)
            if error:
                raise BoardError(error)
            return types.SimpleNamespace(text=reply_text)

        def close(self):
            self.closed = True

    mod.BoardError = BoardError
    mod.Board = Board
    return mod


def test_sdk_tools_dir_has_chat_py():
    assert (sdk_tools_dir() / "chat.py").is_file()


def test_run_turn_returns_reply_and_forwards_text_pieces():
    chat = make_chat(frames=[{"type": "busy", "on": True}, {"type": "text", "text": "返"},
                             {"type": "text", "text": "事"}, {"type": "done"}])
    link = BoardLink("/dev/x", chat_module=chat)
    pieces = []
    assert link.run_turn("q", pieces.append) == "返事"
    assert pieces == ["返", "事"]


def test_board_is_opened_once():
    chat = make_chat()
    link = BoardLink("/dev/x", chat_module=chat)
    link.run_turn("a", lambda _: None)
    link.run_turn("b", lambda _: None)
    assert chat.Board.opened == ["/dev/x"]


def test_open_returns_status():
    link = BoardLink("/dev/x", chat_module=make_chat())
    assert link.open() == {"board": "M5Stack StickS3"}


def test_voice_turn_busy_maps_to_message():
    link = BoardLink("/dev/x", chat_module=make_chat(error="Muse: BUSY WITH A VOICE TURN"))
    with pytest.raises(LinkError) as e:
        link.run_turn("q", lambda _: None)
    assert str(e.value) == MSG_BUSY


def test_other_error_reopens_next_time():
    chat = make_chat(error="The board restarted.")
    link = BoardLink("/dev/x", chat_module=chat)
    with pytest.raises(LinkError) as e:
        link.run_turn("q", lambda _: None)
    assert str(e.value) == "Muse に届きませんでした (The board restarted.)。"
    with pytest.raises(LinkError):
        link.run_turn("q", lambda _: None)
    assert chat.Board.opened == ["/dev/x", "/dev/x"]


def test_long_error_is_shortened_to_first_line():
    chat = make_chat(error="The reply stopped coming. " + "x" * 200 + "\nBoard log: ...")
    link = BoardLink("/dev/x", chat_module=chat)
    with pytest.raises(LinkError) as e:
        link.run_turn("q", lambda _: None)
    msg = str(e.value)
    assert "Board log" not in msg and len(msg) < 100
