"""OpenAI-compatible chat completions for Even AI's custom agent."""
import hmac
import json
import time
import uuid
from datetime import datetime
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from typing import Optional

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


SECRET_NAME_PARTS = ("auth", "key", "token", "cookie")
KNOWN_SCHEMES = ("Bearer ", "Basic ")


def _redact_value(value: str) -> str:
    for scheme in KNOWN_SCHEMES:
        if value.startswith(scheme):
            return f"{scheme}<redacted len={len(value) - len(scheme)}>"
    return f"<redacted len={len(value)}>"


def redact_headers(headers) -> dict:
    out = {}
    for k, v in headers.items():
        secret = any(part in k.lower() for part in SECRET_NAME_PARTS)
        out[k] = _redact_value(v) if secret else v
    return out


COMPLETION_PATHS = ("/v1/chat/completions", "/chat/completions")
MODELS_PATHS = ("/v1/models", "/models")
MODEL_ID = "muse"


def save_capture(capture_dir: Path, method: str, path: str, headers, raw: bytes) -> None:
    capture_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    rec = {"method": method, "path": path, "headers": redact_headers(headers),
           "body": raw.decode("utf-8", "replace")}
    (capture_dir / f"{stamp}.json").write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                                               encoding="utf-8")


def make_handler(turns, token: str, capture_dir: Optional[Path] = None):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _read_body(self) -> bytes:
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length) if length > 0 else b""

        def _send(self, status: int, ctype: str, payload: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def _send_json(self, status: int, obj) -> None:
            self._send(status, "application/json; charset=utf-8",
                       json.dumps(obj, ensure_ascii=False).encode("utf-8"))

        def _authorized(self) -> bool:
            return check_auth(self.headers.get("Authorization", ""), token)

        def do_GET(self) -> None:
            path = self.path.split("?", 1)[0]
            if capture_dir is not None:
                save_capture(capture_dir, "GET", path, self.headers, b"")
            if path not in MODELS_PATHS:
                self._send_json(404, {"error": {"message": "not found"}})
            elif not self._authorized():
                self._send_json(401, {"error": {"message": "unauthorized"}})
            else:
                self._send_json(200, {"object": "list", "data": [
                    {"id": MODEL_ID, "object": "model", "created": 0, "owned_by": "muse-relay"}]})

        def do_POST(self) -> None:
            path = self.path.split("?", 1)[0]
            raw = self._read_body()
            if capture_dir is not None:
                save_capture(capture_dir, "POST", path, self.headers, raw)
            if path not in COMPLETION_PATHS:
                self._send_json(404, {"error": {"message": "not found"}})
                return
            if not self._authorized():
                self._send_json(401, {"error": {"message": "unauthorized"}})
                return
            try:
                body = json.loads(raw)
                prompt = extract_prompt(body)
            except (ValueError, BadRequest) as e:
                self._send_json(400, {"error": {"message": str(e)}})
                return
            reply = clip(turns.ask(prompt))
            model = body.get("model") or MODEL_ID
            created = int(time.time())
            cid = "chatcmpl-" + uuid.uuid4().hex
            if body.get("stream"):
                self._send(200, "text/event-stream; charset=utf-8",
                           completion_sse(reply, model, created, cid))
            else:
                self._send_json(200, completion_json(reply, model, created, cid))

    return Handler
