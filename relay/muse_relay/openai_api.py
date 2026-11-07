"""OpenAI-compatible chat completions for Even AI's custom agent."""
import hmac
import json

MAX_CHARS = 400        # what fits on the Even AI card


class BadRequest(Exception):
    pass


def _content_text(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(p.get("text", "") for p in content
                       if isinstance(p, dict) and p.get("type") == "text")
    return ""


def extract_prompt(body: dict) -> str:
    """The last user message's text: Muse keeps its own history."""
    messages = body.get("messages")
    if not isinstance(messages, list):
        raise BadRequest("messages missing")
    for m in reversed(messages):
        if isinstance(m, dict) and m.get("role") == "user":
            text = _content_text(m.get("content")).strip()
            if text:
                return text
    raise BadRequest("no user message")


def clip(text: str, limit: int = MAX_CHARS) -> str:
    return text if len(text) <= limit else text[:limit - 1] + "…"


def check_auth(header: str, token: str) -> bool:
    prefix = "Bearer "
    if not header.startswith(prefix):
        return False
    return hmac.compare_digest(header[len(prefix):].encode(), token.encode())


def completion_json(text: str, model: str, created: int, cid: str) -> dict:
    return {
        "id": cid,
        "object": "chat.completion",
        "created": created,
        "model": model,
        "choices": [{"index": 0, "message": {"role": "assistant", "content": text},
                     "finish_reason": "stop"}],
        "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
    }


def completion_sse(text: str, model: str, created: int, cid: str) -> bytes:
    def chunk(delta: dict, finish) -> str:
        c = {"id": cid, "object": "chat.completion.chunk", "created": created, "model": model,
             "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
        return "data: " + json.dumps(c, ensure_ascii=False) + "\n\n"
    out = chunk({"role": "assistant", "content": text}, None) + chunk({}, "stop") + "data: [DONE]\n\n"
    return out.encode("utf-8")


def redact_headers(headers) -> dict:
    out = {}
    for k, v in headers.items():
        if k.lower() == "authorization":
            scheme, _, cred = v.partition(" ")
            out[k] = f"{scheme} <redacted len={len(cred)}>"
        else:
            out[k] = v
    return out
