"""Muse widgets (the board's "presentation" frames) as plain text, the same way the hub renders them
(hub/components/hub/hub_proto.c and hub_turns.c): keep the two in step."""
import json
import re
from typing import Callable, Iterable, Optional

from .openai_api import BLANK_CHARS

UNKNOWN_WIDGET = "(Muse アプリに表示: 選択肢など)"
SHOWN_HEAD = "(Muse アプリに表示: "
SHOWN_TAIL = ")"
WIDGET_LABEL = "ウィジェット"
IMAGE_OPTIONS_LABEL = "画像の選択肢"
ITEM_MARK = "・"
SUBTITLE_SEP = " — "
MARKER_HEAD = "[[hatch_widget:"
MARKER_TAIL = "]]"
WIDGET_ID_MAX = 128        # bytes of an id, terminator included, as on the hub
WIDGET_MAX = 16            # widgets kept per turn; later ones are dropped
WIDGET_JSON_MAX = 16384    # bytes of a widget's JSON; a longer one shows as a bare note

_SPACES = re.compile(r"[ \t\r\n]+")


def clean(text: str) -> str:
    """text without "**", each run of spaces and newlines as one space, trimmed."""
    return _SPACES.sub(" ", text.replace("**", "")).strip(" \t\r\n")


def _str(obj, key: str) -> str:
    v = obj.get(key) if isinstance(obj, dict) else None
    return v if isinstance(v, str) else ""


def _obj(obj, key: str) -> dict:
    v = obj.get(key) if isinstance(obj, dict) else None
    return v if isinstance(v, dict) else {}


def _array(obj, key: str) -> list:
    v = obj.get(key) if isinstance(obj, dict) else None
    return v if isinstance(v, list) else []


def _item(item) -> Optional[str]:
    """"・title — subtitle (tertiary_title)"; None for an item without a title."""
    title = clean(_str(item, "title"))
    if not title:
        return None
    line = ITEM_MARK + title
    subtitle = clean(_str(item, "subtitle"))
    if subtitle:
        line += SUBTITLE_SEP + subtitle
    tertiary = clean(_str(item, "tertiary_title"))
    if tertiary:
        line += f" ({tertiary})"
    return line


def _list(title: str, display: str, items: list, markers_only: bool) -> list[str]:
    header = clean(title) or clean(display.split("\n")[0])
    lines = [header] if header else []
    for item in items:
        if markers_only and _str(item, "kind") != "marker":
            continue
        line = _item(item)
        if line:
            lines.append(line)
    return lines


def _parse(text: str):
    try:
        return json.loads(text)
    except (ValueError, RecursionError):
        return None


def render(text: str) -> str:
    """A widget's JSON as plain text: a list or map line by line, anything else as a note."""
    root = _parse(text)
    root = root if isinstance(root, dict) else {}
    kind = _str(root, "kind")
    display = _str(root, "display_text")
    data = _obj(root, "data")
    lines: list[str] = []
    if kind == "generic_list":
        lines = _list(_str(data, "title"), display, _array(data, "items"), False)
    elif kind == "local_map":
        lines = _list("", display, _array(_obj(data, "typed_data"), "elements"), True)
    if lines:
        return "\n".join(lines)
    label = IMAGE_OPTIONS_LABEL if kind == "image_options" else WIDGET_LABEL
    return SHOWN_HEAD + (clean(display) or clean(_str(root, "fallback_text")) or label) + SHOWN_TAIL


def replace_markers(text: str, lookup: Callable[[str], Optional[str]]) -> str:
    """text with each "[[hatch_widget:<id>]]" replaced by the widget lookup(id) gives, or UNKNOWN_WIDGET.
    A marker without its "]]", or with an empty or too long id, stays as it is."""
    out = []
    p = 0
    while (m := text.find(MARKER_HEAD, p)) >= 0:
        out.append(text[p:m])
        start = m + len(MARKER_HEAD)
        end = text.find(MARKER_TAIL, start)
        if end < 0 or not 0 < len(text[start:end].encode("utf-8")) < WIDGET_ID_MAX:
            out.append(MARKER_HEAD)
            p = start
            continue
        found = lookup(text[start:end])
        out.append(render(found) if found is not None else UNKNOWN_WIDGET)
        p = end + len(MARKER_TAIL)
    out.append(text[p:])
    return "".join(out)


class Widgets:
    """One turn's widgets, in arrival order. The board splits a long one over frames with the same id."""

    def __init__(self) -> None:
        self._json: dict[str, str] = {}

    def feed(self, frame: dict) -> None:
        wid, text = frame.get("id"), frame.get("text")
        if frame.get("type") != "presentation" or not isinstance(wid, str) or not wid or not isinstance(text, str):
            return
        so_far = self._json.get(wid)
        if so_far is not None and _parse(so_far) is None:
            self._json[wid] = so_far + text      # the rest of a split one
        elif so_far is not None or len(self._json) < WIDGET_MAX:
            self._json[wid] = text               # a repeat replaces it in place

    def _get(self, wid: str) -> str:
        text = self._json[wid]
        return text if len(text.encode("utf-8")) < WIDGET_JSON_MAX else ""

    def compose(self, messages: Iterable[str]) -> str:
        """The messages that are not blank, with their markers replaced, then the widgets no marker named,
        joined by blank lines."""
        named = set()

        def lookup(wid: str) -> Optional[str]:
            if wid not in self._json:
                return None
            named.add(wid)
            return self._get(wid)

        parts = [replace_markers(m, lookup) for m in messages if m.strip(BLANK_CHARS)]
        parts += [render(self._get(wid)) for wid in self._json if wid not in named]
        return "\n\n".join(parts)
