import json

from muse_relay.widgets import UNKNOWN_WIDGET, WIDGET_MAX, Widgets, render, replace_markers

# The widgets seen on the device, as the SDK forwards them (the HTML dropped). Same as hub/test/host/test_hub.c.
WIDGET_LIST = {
    "id": "widget-0ed9", "kind": "generic_list",
    "display_text": "**リスト型の見本：ウィジェットの種類一覧**\n- 選択肢型\n- 地図型\n- リスト型",
    "data": {"title": "リスト型の見本：ウィジェットの種類一覧", "items": [
        {"title": "選択肢型", "subtitle": "タップで返信になるボタン", "tertiary_title": "任意", "type": "generic",
         "data": {}, "icons": []},
        {"title": "地図型", "subtitle": "", "type": "generic", "data": {}, "icons": []},
        {"title": "リスト型", "subtitle": "項目の一覧", "type": "generic", "data": {}, "icons": []}]}}
WIDGET_LIST_TEXT = ("リスト型の見本：ウィジェットの種類一覧\n"
                    "・選択肢型 — タップで返信になるボタン (任意)\n"
                    "・地図型\n"
                    "・リスト型 — 項目の一覧")
WIDGET_MAP = {
    "id": "widget-e172", "kind": "local_map", "display_text": "地図型の見本：アイーナとパシフィコ横浜",
    "data": {"typed_data": {"kind": "local_map", "elements": [
        {"kind": "marker", "title": "アイーナ（NTいわて会場）", "subtitle": "明日11/8 12:00〜16:00",
         "coordinate": {"latitude": 39.7017, "longitude": 141.1365}},
        {"kind": "route", "title": "経路"},
        {"kind": "marker", "title": "パシフィコ横浜", "subtitle": "11/9 10:00〜17:00",
         "coordinate": {"latitude": 35.4575, "longitude": 139.6375}}]}}}
WIDGET_MAP_TEXT = ("地図型の見本：アイーナとパシフィコ横浜\n"
                   "・アイーナ（NTいわて会場） — 明日11/8 12:00〜16:00\n"
                   "・パシフィコ横浜 — 11/9 10:00〜17:00")
WIDGET_HTML = {"id": "widget-3572", "kind": "html", "fallback_text": "サイコロ見本ウィジェット",
               "data": {"fallback_text": "サイコロ見本ウィジェット"}}
WIDGET_IMAGES = {"id": "widget-ce86", "kind": "image_options", "display_text": "Choose from ⁨4⁩ image options",
                 "data": {"images": [{"path": "a.png"}, {"path": "b.png"}, {"path": "c.png"}, {"path": "d.png"}],
                          "multi_select": False}}


def js(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def test_render_list_and_map():
    assert render(js(WIDGET_LIST)) == WIDGET_LIST_TEXT
    assert render(js(WIDGET_MAP)) == WIDGET_MAP_TEXT
    assert render(js({"kind": "generic_list", "display_text": "**見出し**\n- a",
                      "data": {"items": [{"title": " a \n b "}, {"subtitle": "x"},
                                         {"title": "c", "tertiary_title": "t"}]}})) == "見出し\n・a b\n・c (t)"


def test_render_other_kinds_as_a_note():
    assert render(js(WIDGET_HTML)) == "(Muse アプリに表示: サイコロ見本ウィジェット)"
    assert render(js(WIDGET_IMAGES)) == "(Muse アプリに表示: Choose from ⁨4⁩ image options)"
    assert render('{"kind":"image_options"}') == "(Muse アプリに表示: 画像の選択肢)"
    assert render(js({"kind": "weather", "display_text": "**晴れ**\n\n 22℃ "})) == "(Muse アプリに表示: 晴れ 22℃)"
    assert render('{"kind":"generic_list","data":{"items":[]}}') == "(Muse アプリに表示: ウィジェット)"
    for bad in ("not json", '{"kind":"html"} x', "", "[1]"):
        assert render(bad) == "(Muse アプリに表示: ウィジェット)"


def test_replace_markers():
    seen = []

    def lookup(wid):
        seen.append(wid)
        return js(WIDGET_HTML) if wid == "widget-A" else None

    text = "見本です。\n\n[[hatch_widget:widget-A]]\n[[hatch_widget:widget-B]]終わり"
    assert replace_markers(text, lookup) == ("見本です。\n\n(Muse アプリに表示: サイコロ見本ウィジェット)\n"
                                             "(Muse アプリに表示: 選択肢など)終わり")
    assert seen == ["widget-A", "widget-B"]
    assert UNKNOWN_WIDGET == "(Muse アプリに表示: 選択肢など)"
    text = "a [[hatch_widget:]] b [[hatch_widget:widget-A"
    assert replace_markers(text, lookup) == text
    assert seen == ["widget-A", "widget-B"]


DICE = js({"id": "widget-A", "kind": "html", "fallback_text": "サイコロ", "data": {}})
DICE_TEXT = "(Muse アプリに表示: サイコロ)"
LIST = js({"id": "widget-B", "kind": "generic_list", "data": {"title": "一覧", "items": [{"title": "a"}]}})
LIST_TEXT = "一覧\n・a"


def pres(wid: str, text: str) -> dict:
    return {"type": "presentation", "id": wid, "text": text}


def test_compose_replaces_markers_and_appends_the_rest():
    w = Widgets()
    w.feed(pres("widget-A", DICE))
    w.feed(pres("widget-B", LIST))
    assert w.compose(["見本です。\n\n[[hatch_widget:widget-A]]", "[[hatch_widget:widget-X]]"]) == (
        "見本です。\n\n" + DICE_TEXT + "\n\n" + UNKNOWN_WIDGET + "\n\n" + LIST_TEXT)


def test_compose_skips_blank_messages():
    w = Widgets()
    w.feed(pres("widget-B", LIST))
    w.feed(pres("widget-A", DICE))
    assert w.compose(["", "\n　"]) == LIST_TEXT + "\n\n" + DICE_TEXT
    assert Widgets().compose(["", "\n"]) == ""


def test_split_frames_are_put_back_together_and_a_repeat_replaces():
    w = Widgets()
    w.feed(pres("widget-A", '{"kind":"html"}'))
    w.feed(pres("widget-B", LIST[:10]))
    w.feed(pres("widget-B", LIST[10:30]))
    w.feed(pres("widget-B", LIST[30:]))
    w.feed(pres("widget-A", DICE[:7]))
    w.feed(pres("widget-A", DICE[7:]))
    w.feed({"type": "text", "msg": 0, "text": "x"})
    assert w.compose(["本文"]) == "本文\n\n" + DICE_TEXT + "\n\n" + LIST_TEXT


def test_widget_count_and_size_are_capped():
    w = Widgets()
    for i in range(WIDGET_MAX + 4):
        w.feed(pres(f"widget-{i}", '{"kind":"html","fallback_text":"w"}'))
    assert w.compose([]).count("(Muse") == WIDGET_MAX
    w = Widgets()
    w.feed(pres("widget-A", js({"kind": "html", "fallback_text": "あ" * 6000})))
    assert w.compose([]) == "(Muse アプリに表示: ウィジェット)"
