import types

import pytest

from muse_relay.board_link import MSG_BUSY, MSG_INTERNAL, BoardLink, _load_chat, sdk_tools_dir
from muse_relay.turns import LinkError


def make_chat(frames=(), reply_text="返事", error=None, exc=None):
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
            if exc:
                raise exc
            if error:
                raise BoardError(error)
            return types.SimpleNamespace(text=reply_text)

        def close(self):
            self.closed = True

    mod.BoardError = BoardError
    mod.Reply = _load_chat().Reply          # the SDK's own accumulator
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
    assert pieces == ["返", "返事"]          # each call carries the whole partial text so far


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


def test_unexpected_exception_maps_to_link_error_and_closes():
    chat = make_chat(exc=ValueError("surrogates not allowed"))
    link = BoardLink("/dev/x", chat_module=chat)
    with pytest.raises(LinkError) as e:
        link.run_turn("q", lambda _: None)
    assert str(e.value) == MSG_INTERNAL
    with pytest.raises(LinkError):
        link.run_turn("q", lambda _: None)
    assert chat.Board.opened == ["/dev/x", "/dev/x"]


def test_muse_side_error_keeps_the_port_open():
    chat = make_chat(error="Muse: network error")
    link = BoardLink("/dev/x", chat_module=chat)
    for _ in range(2):
        with pytest.raises(LinkError) as e:
            link.run_turn("q", lambda _: None)
        assert str(e.value) == "Muse に届きませんでした (Muse: network error)。"
    assert chat.Board.opened == ["/dev/x"]


def test_final_replaces_streamed_pieces_and_messages_are_joined():
    chat = make_chat(frames=[{"type": "text", "msg": 1, "text": "ab"},
                             {"type": "final", "msg": 1, "text": "ABC"},
                             {"type": "text", "msg": 2, "text": "d"}])
    link = BoardLink("/dev/x", chat_module=chat)
    seen = []
    link.run_turn("q", seen.append)
    assert seen == ["ab", "ABC", "ABC\n\nd"]


DICE = '{"id":"widget-A","kind":"html","fallback_text":"サイコロ","data":{}}'
LIST = '{"id":"widget-B","kind":"generic_list","data":{"title":"一覧","items":[{"title":"a"}]}}'


def test_widgets_are_rendered_into_the_reply():
    chat = make_chat(frames=[{"type": "presentation", "id": "widget-B", "text": LIST[:20]},
                             {"type": "presentation", "id": "widget-B", "text": LIST[20:]},
                             {"type": "text", "msg": 0, "text": "見本です。\n\n[[hatch_widget:widget-A]]"},
                             {"type": "presentation", "id": "widget-A", "text": DICE},
                             {"type": "text", "msg": 1, "text": ""},
                             {"type": "done"}])
    link = BoardLink("/dev/x", chat_module=chat)
    seen = []
    assert link.run_turn("q", seen.append) == "見本です。\n\n(Muse アプリに表示: サイコロ)\n\n一覧\n・a"
    assert seen[0] == "見本です。\n\n(Muse アプリに表示: 選択肢など)\n\n一覧\n・a"   # widget-A not there yet


def test_only_widgets_and_blank_messages():
    chat = make_chat(frames=[{"type": "presentation", "id": "widget-B", "text": LIST},
                             {"type": "text", "msg": 0, "text": ""},
                             {"type": "presentation", "id": "widget-A", "text": DICE},
                             {"type": "text", "msg": 1, "text": "\n"},
                             {"type": "done"}])
    link = BoardLink("/dev/x", chat_module=chat)
    assert link.run_turn("q", lambda _: None) == "一覧\n・a\n\n(Muse アプリに表示: サイコロ)"


def test_no_text_and_no_widgets_is_empty():
    chat = make_chat(frames=[{"type": "text", "msg": 0, "text": ""}, {"type": "done"}])
    assert BoardLink("/dev/x", chat_module=chat).run_turn("q", lambda _: None) == ""
