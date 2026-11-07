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
