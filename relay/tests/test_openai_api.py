import json

import pytest

from muse_relay.openai_api import (ALL_MAX_CHARS, MAX_CHARS, BadRequest, check_auth, clip, completion_json,
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


def test_redact_headers_hides_every_credential_shaped_header():
    out = redact_headers({"Authorization": "bare-secret", "x-api-key": "k-secret", "api-key": "k2-secret",
                          "Cookie": "sid=c-secret", "Authorization-Extra": "Basic b-secret"})
    dumped = json.dumps(out)
    for secret in ("bare-secret", "k-secret", "k2-secret", "c-secret", "b-secret"):
        assert secret not in dumped
    assert out["Authorization"] == "<redacted len=11>"
    assert out["Authorization-Extra"] == "Basic <redacted len=8>"


import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

import socket

from muse_relay.openai_api import MAX_BODY, MAX_REJECTED_CAPTURES, make_handler

TOKEN = "x" * 40


class FakeTurns:
    def __init__(self, reply="返事"):
        self.reply = reply
        self.asked = []

    def ask(self, text):
        self.asked.append(text)
        return self.reply


def _raw(url, request: bytes) -> bytes:
    port = int(url.rsplit(":", 1)[1])
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(request)
        data = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data


@pytest.fixture
def server(tmp_path):
    turns = FakeTurns()
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(turns, TOKEN, tmp_path / "captures"))
    t = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}", turns, tmp_path / "captures"
    httpd.shutdown()
    httpd.server_close()


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
    turns.reply = "あ" * (ALL_MAX_CHARS + 1000)
    d = json.loads(_post(url, USER)[2])
    assert len(d["choices"][0]["message"]["content"]) == ALL_MAX_CHARS


def test_reply_of_the_whole_rest_is_not_clipped_at_a_page(server):
    url, turns, _ = server
    turns.reply = "あ" * (MAX_CHARS + 1000)          # 「終わりまで」 returns up to ALL_MAX_CHARS
    d = json.loads(_post(url, USER)[2])
    assert d["choices"][0]["message"]["content"] == turns.reply


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


def test_rejected_requests_are_captured_without_body_or_token(server):
    url, turns, captures = server
    _post(url, USER, token="nope-secret-token")
    _post(url, USER, path="/v1/other")
    recs = sorted((json.loads(f.read_text(encoding="utf-8")) for f in captures.glob("*.json")),
                  key=lambda r: r["rejected"])
    assert [r["rejected"] for r in recs] == [401, 404]
    assert all("body" not in r for r in recs)
    assert "nope-secret-token" not in json.dumps(recs)
    assert turns.asked == []


def test_rejected_captures_are_capped(server):
    url, _, captures = server
    for _ in range(MAX_REJECTED_CAPTURES + 5):
        _post(url, USER, token="nope")
    assert len(list(captures.glob("*.json"))) == MAX_REJECTED_CAPTURES


@pytest.mark.parametrize("size_line", [b"-1", b"0x10", b"1_0", b"5;" + b"e" * 100])
def test_bad_chunk_size_is_400(server, size_line):
    url, turns, _ = server
    req = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n").encode() \
        + size_line + b"\r\nhello\r\n0\r\n\r\n"
    assert _raw(url, req).startswith(b"HTTP/1.1 400")
    assert turns.asked == []


def test_get_with_body_closes_the_connection(server):
    url, _, _ = server
    req = (f"GET /v1/models HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           "Content-Length: 5\r\n\r\nhello").encode()
    assert _raw(url, req).startswith(b"HTTP/1.1 200")      # _raw reads until the server closes


def test_extract_prompt_replaces_lone_surrogates():
    body = {"messages": [{"role": "user", "content": "a\ud800b"}]}
    prompt = extract_prompt(body)
    assert prompt == "a?b"
    prompt.encode("utf-8")


def test_oversized_content_length_is_413_without_reading(server):
    url, turns, captures = server
    head = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
            f"Content-Length: {MAX_BODY + 1}\r\n\r\n").encode()
    port = int(url.rsplit(":", 1)[1])
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(head)
        resp = b""
        while True:                        # reads to EOF: the server must close the connection
            part = sock.recv(4096)
            if not part:
                break
            resp += part
        assert resp.startswith(b"HTTP/1.1 413")
    assert turns.asked == []


def test_non_numeric_content_length_is_400(server):
    url, _, _ = server
    req = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           "Content-Length: abc\r\n\r\n").encode()
    assert _raw(url, req).startswith(b"HTTP/1.1 400")


@pytest.mark.parametrize("payload", [b"[]", b'"x"', b"123"])
def test_non_object_json_is_400(server, payload):
    url, _, _ = server
    req = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n").encode() + payload
    assert _raw(url, req).startswith(b"HTTP/1.1 400")


def test_chunked_body_is_decoded(server):
    url, turns, _ = server
    body = json.dumps(USER, ensure_ascii=False).encode("utf-8")
    mid = len(body) // 2
    chunked = b"".join(b"%x\r\n%s\r\n" % (len(c), c) for c in (body[:mid], body[mid:])) + b"0\r\nX-T: 1\r\n\r\n"
    req = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n").encode() + chunked
    resp = _raw(url, req)
    assert resp.startswith(b"HTTP/1.1 200")
    assert turns.asked == ["こんにちは"]


def test_oversized_chunked_body_is_413(server):
    url, turns, _ = server
    big = b"a" * (MAX_BODY + 1)
    chunked = b"%x\r\n%s\r\n0\r\n\r\n" % (len(big), big)
    req = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n").encode() + chunked
    assert _raw(url, req).startswith(b"HTTP/1.1 413")
    assert turns.asked == []


def test_post_without_length_or_chunked_is_400(server):
    url, _, _ = server
    req = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {TOKEN}\r\n"
           "Connection: close\r\n\r\n").encode()
    assert _raw(url, req).startswith(b"HTTP/1.1 400")


@pytest.mark.parametrize("token", ["", "short", "x" * 31])
def test_short_token_is_refused(token):
    with pytest.raises(ValueError):
        make_handler(FakeTurns(), token)


def _get(url, path, token=TOKEN):
    headers = {"Authorization": "Bearer " + token} if token else {}
    req = urllib.request.Request(url + path, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")


@pytest.mark.parametrize("path", ["/v1/chat/completions", "/chat/completions"])
def test_get_on_completions_answers_ok_when_authorized(server, path):
    url, turns, _ = server
    status, body = _get(url, path)
    assert status == 200
    assert json.loads(body)["status"] == "ok"
    assert turns.asked == []


def test_get_on_completions_without_token_is_401(server):
    url, _, _ = server
    assert _get(url, "/v1/chat/completions", token="")[0] == 401


def test_get_on_completions_is_captured(server):
    url, _, captures = server
    _get(url, "/v1/chat/completions")
    recs = [json.loads(f.read_text(encoding="utf-8")) for f in captures.glob("*.json")]
    assert [(r["method"], r["path"]) for r in recs] == [("GET", "/v1/chat/completions")]


def test_scanner_paths_are_not_captured(server):
    url, _, captures = server
    for path in ("/", "/.env", "/wp-login.php", "/actuator/env"):
        _get(url, path, token="")
    assert list(captures.glob("*.json")) == []


def test_rejected_api_paths_are_still_captured(server):
    url, _, captures = server
    _get(url, "/v1/unknown", token="")
    _get(url, "/v1/chat/completions", token="")
    recs = sorted((json.loads(f.read_text(encoding="utf-8")) for f in captures.glob("*.json")),
                  key=lambda r: r["rejected"])
    assert [r["rejected"] for r in recs] == [401, 404]
