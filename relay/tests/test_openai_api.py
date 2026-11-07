import json

import pytest

from muse_relay.openai_api import (MAX_CHARS, BadRequest, check_auth, clip, completion_json,
                                   completion_sse, extract_prompt, redact_headers)


def test_extract_prompt_takes_last_user_message():
    body = {"messages": [{"role": "system", "content": "s"},
                         {"role": "user", "content": "一つ目"},
                         {"role": "assistant", "content": "a"},
                         {"role": "user", "content": " 二つ目 "}]}
    assert extract_prompt(body) == "二つ目"


def test_extract_prompt_joins_text_parts():
    body = {"messages": [{"role": "user", "content": [{"type": "text", "text": "電気"},
                                                      {"type": "image_url", "image_url": {}},
                                                      {"type": "text", "text": "消して"}]}]}
    assert extract_prompt(body) == "電気消して"


@pytest.mark.parametrize("body", [{}, {"messages": "x"}, {"messages": [{"role": "system", "content": "s"}]},
                                  {"messages": [{"role": "user", "content": "  "}]}])
def test_extract_prompt_rejects_missing_user_text(body):
    with pytest.raises(BadRequest):
        extract_prompt(body)


def test_clip():
    assert clip("あ" * MAX_CHARS) == "あ" * MAX_CHARS
    clipped = clip("あ" * (MAX_CHARS + 5))
    assert len(clipped) == MAX_CHARS and clipped.endswith("…")


def test_check_auth():
    token = "t" * 40
    assert check_auth("Bearer " + token, token)
    assert not check_auth("Bearer wrong", token)
    assert not check_auth(token, token)
    assert not check_auth("", token)


def test_completion_json_shape():
    d = completion_json("返事", "muse", 123, "chatcmpl-x")
    assert d["object"] == "chat.completion"
    assert d["choices"][0]["message"] == {"role": "assistant", "content": "返事"}
    assert d["choices"][0]["finish_reason"] == "stop"
    assert d["model"] == "muse" and d["created"] == 123 and d["id"] == "chatcmpl-x"


def test_completion_sse_shape():
    raw = completion_sse("返事", "muse", 123, "chatcmpl-x").decode("utf-8")
    events = [e for e in raw.split("\n\n") if e]
    assert events[-1] == "data: [DONE]"
    chunks = [json.loads(e[len("data: "):]) for e in events[:-1]]
    assert all(c["object"] == "chat.completion.chunk" for c in chunks)
    assert chunks[0]["choices"][0]["delta"] == {"role": "assistant", "content": "返事"}
    assert chunks[-1]["choices"][0]["finish_reason"] == "stop"


def test_redact_headers_hides_authorization_value():
    out = redact_headers({"Authorization": "Bearer secret-value", "Content-Type": "application/json"})
    assert "secret-value" not in json.dumps(out)
    assert out["Authorization"] == "Bearer <redacted len=12>"
    assert out["Content-Type"] == "application/json"


import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

from muse_relay.openai_api import make_handler

TOKEN = "x" * 40


class FakeTurns:
    def __init__(self, reply="返事"):
        self.reply = reply
        self.asked = []

    def ask(self, text):
        self.asked.append(text)
        return self.reply


@pytest.fixture
def server(tmp_path):
    turns = FakeTurns()
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(turns, TOKEN, tmp_path / "captures"))
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}", turns, tmp_path / "captures"
    httpd.shutdown()


def _post(url, body, token=TOKEN, path="/v1/chat/completions"):
    req = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.headers.get("Content-Type"), r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type"), e.read().decode("utf-8")


USER = {"model": "m1", "messages": [{"role": "user", "content": "こんにちは"}]}


def test_post_returns_json_completion(server):
    url, turns, _ = server
    status, ctype, body = _post(url, USER)
    assert status == 200 and ctype.startswith("application/json")
    d = json.loads(body)
    assert d["choices"][0]["message"]["content"] == "返事"
    assert d["model"] == "m1"
    assert turns.asked == ["こんにちは"]


def test_post_without_v1_prefix_also_works(server):
    url, _, _ = server
    assert _post(url, USER, path="/chat/completions")[0] == 200


def test_stream_returns_sse(server):
    url, _, _ = server
    status, ctype, body = _post(url, dict(USER, stream=True))
    assert status == 200 and ctype.startswith("text/event-stream")
    assert body.rstrip().endswith("data: [DONE]")


def test_bad_token_is_401_and_not_asked(server):
    url, turns, _ = server
    assert _post(url, USER, token="nope")[0] == 401
    assert turns.asked == []


def test_bad_body_is_400(server):
    url, _, _ = server
    assert _post(url, {"messages": []})[0] == 400


def test_unknown_path_is_404(server):
    url, _, _ = server
    assert _post(url, USER, path="/v1/other")[0] == 404


def test_models_lists_one_model(server):
    url, _, _ = server
    req = urllib.request.Request(url + "/v1/models", headers={"Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=5) as r:
        d = json.loads(r.read())
    assert d["object"] == "list" and d["data"][0]["id"] == "muse"


def test_reply_is_clipped(server):
    url, turns, _ = server
    turns.reply = "あ" * 1000
    d = json.loads(_post(url, USER)[2])
    assert len(d["choices"][0]["message"]["content"]) == MAX_CHARS


def test_capture_saves_request_without_token(server):
    url, _, captures = server
    _post(url, USER)
    files = list(captures.glob("*.json"))
    assert len(files) == 1
    saved = files[0].read_text(encoding="utf-8")
    assert TOKEN not in saved
    rec = json.loads(saved)
    assert rec["method"] == "POST" and rec["path"] == "/v1/chat/completions"
    assert json.loads(rec["body"])["messages"][0]["content"] == "こんにちは"
