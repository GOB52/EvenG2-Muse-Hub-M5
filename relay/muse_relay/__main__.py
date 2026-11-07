"""python -m muse_relay --port /dev/cu.usbmodemXXXX [--listen 127.0.0.1:8787] [--capture]"""
import argparse
import os
import sys
from http.server import ThreadingHTTPServer
from pathlib import Path

from .board_link import BoardLink
from .openai_api import make_handler
from .turns import TurnManager

RELAY_DIR = Path(__file__).resolve().parents[1]
ENV_FILE = RELAY_DIR / ".env"
CAPTURE_DIR = RELAY_DIR / "captures"
TOKEN_MIN = 32


def load_token(env_file: Path) -> str:
    token = os.environ.get("RELAY_TOKEN", "")
    if not token and env_file.is_file():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip() == "RELAY_TOKEN":
                token = value.strip().strip('"')
    return token


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="muse_relay", description=__doc__)
    ap.add_argument("--port", required=True, help="the board's serial port")
    ap.add_argument("--listen", default="127.0.0.1:8787", help="host:port to serve on")
    ap.add_argument("--capture", action="store_true", help="save each request to relay/captures/")
    args = ap.parse_args(argv)

    token = load_token(ENV_FILE)
    if len(token) < TOKEN_MIN:
        print(f"RELAY_TOKEN ({TOKEN_MIN}+ chars) is not set in {ENV_FILE}", file=sys.stderr)
        return 2
    host, _, port = args.listen.rpartition(":")

    link = BoardLink(args.port)
    try:
        status = link.open()
    except Exception as e:      # BoardError / OSError from opening the port
        print(f"can't open the board on {args.port}: {e}", file=sys.stderr)
        return 1
    if status is None:
        print(f"no answer from the board on {args.port}", file=sys.stderr)
        return 1
    print(f"board: {status.get('board')}", file=sys.stderr)

    handler = make_handler(TurnManager(link), token, CAPTURE_DIR if args.capture else None)
    server = ThreadingHTTPServer((host, int(port)), handler)
    print(f"listening on http://{host}:{port}" + (" (capturing)" if args.capture else ""), file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
