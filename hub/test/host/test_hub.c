#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "hub_proto.h"

static int s_failed;
static int s_run;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            s_failed++;                                                      \
        }                                                                    \
    } while (0)

#define RUN(fn)            \
    do {                   \
        s_run++;           \
        fn();              \
    } while (0)

static hub_req_err_t parse(const char *json, hub_request_t *req)
{
    return hub_proto_parse_request(json, strlen(json), req);
}

static void test_parse_takes_last_user_message(void)
{
    hub_request_t req;
    CHECK(parse("{\"model\":\"openclaw\",\"messages\":[{\"role\":\"user\",\"content\":\"一つ目\"},"
                "{\"role\":\"assistant\",\"content\":\"a\"},{\"role\":\"user\",\"content\":\" 二つ目 \"}]}",
                &req) == HUB_REQ_OK);
    CHECK(strcmp(req.prompt, "二つ目") == 0);
    CHECK(strcmp(req.model, "openclaw") == 0);
    CHECK(!req.stream);
}

static void test_parse_joins_text_parts_and_reads_stream(void)
{
    hub_request_t req;
    CHECK(parse("{\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"電気\"},"
                "{\"type\":\"image_url\"},{\"type\":\"text\",\"text\":\"消して\"}]}]}",
                &req) == HUB_REQ_OK);
    CHECK(strcmp(req.prompt, "電気消して") == 0);
    CHECK(req.stream);
    CHECK(strcmp(req.model, "muse") == 0);     /* default when absent */
}

static void test_parse_rejects_bad_requests(void)
{
    hub_request_t req;
    CHECK(parse("not json", &req) == HUB_REQ_BAD_JSON);
    CHECK(parse("[]", &req) == HUB_REQ_BAD_JSON);
    CHECK(parse("{}", &req) == HUB_REQ_NO_USER);
    CHECK(parse("{\"messages\":[{\"role\":\"system\",\"content\":\"s\"}]}", &req) == HUB_REQ_NO_USER);
    CHECK(parse("{\"messages\":[{\"role\":\"user\",\"content\":\"   \"}]}", &req) == HUB_REQ_NO_USER);
}

static void test_check_auth(void)
{
    const char *token = "0123456789abcdef0123456789abcdef";
    char header[64];
    snprintf(header, sizeof(header), "Bearer %s", token);
    CHECK(hub_proto_check_auth(header, token));
    CHECK(!hub_proto_check_auth("Bearer wrong", token));
    CHECK(!hub_proto_check_auth(token, token));
    CHECK(!hub_proto_check_auth("", token));
    CHECK(!hub_proto_check_auth("Bearer ", ""));
    CHECK(!hub_proto_check_auth("Bearer 0123456789abcdef0123456789abcdef0", token));
}

static void test_utf8_helpers(void)
{
    CHECK(hub_proto_utf8_chars("aあ…") == 3);
    char s[8] = "aあ";
    s[3] = '\0';                               /* cut "あ" after 2 of its 3 bytes */
    hub_proto_utf8_trim(s);
    CHECK(strcmp(s, "a") == 0);
}

static void test_clip(void)
{
    char out[64];
    hub_proto_clip("あいうえお", out, sizeof(out), 5);
    CHECK(strcmp(out, "あいうえお") == 0);
    hub_proto_clip("あいうえおか", out, sizeof(out), 5);
    CHECK(strcmp(out, "あいうえ…") == 0);
    hub_proto_clip("abcdef", out, 5, 10);      /* out too small: stays valid UTF-8 and terminated */
    CHECK(strlen(out) <= 4);
    hub_proto_clip("\xE3" "a", out, sizeof(out), 10);   /* lead byte without its continuation bytes */
    CHECK(strlen(out) == 2);
    hub_proto_clip("\xE3", out, sizeof(out), 10);
    CHECK(strcmp(out, "") == 0);
    hub_proto_clip("\xE3" "a" "\xE3", out, sizeof(out), 1);
    CHECK(strlen(out) < sizeof(out));
}

static void test_page_cut(void)
{
    CHECK(hub_proto_page_cut("あいう", 5) == 9);                     /* fits: the whole text */
    CHECK(hub_proto_page_cut("abcdefghij", 5) == 5);                /* no break: at the room */
    CHECK(hub_proto_page_cut("abcd！efgh", 5) == 7);                /* right after "！" */
    CHECK(hub_proto_page_cut("abcdefgh？ij", 10) == 11);            /* "？" ends char 9 of 10 */
    CHECK(hub_proto_page_cut("a。bcdefghij", 10) == 12);            /* "。" before the last 20% */
    CHECK(hub_proto_page_cut("abcdefgh\nij", 10) == 9);
}

static void test_page_key(void)
{
    static const char *const continues[] = {
        "続き", "続きを見せて。", "　つづき、！", "次", "次のページ", "つぎ", "もっと", "もっと見せて",
    };
    static const char *const alls[] = {
        "終わりまで", "最後まで読んで", "全部お願い", "全文", "続きを全部", "「終わり、まで」…", "さいごまで",
        "続き全部", "残り全部", "残り",
    };
    static const char *const others[] = {
        "手続きの方法は?", "次の予定は?", "次回の会議はいつ", "目次を見せて", "もっと安い店は?", "全部屋の温度は?",
        "つぎはぎ", "続きはどうなった?", "次の東京の天気と明日の大阪の天気を教えてください", "天気は", "",
        "続きの話はまた今度にしましょうね",
    };
    for (size_t i = 0; i < sizeof(continues) / sizeof(continues[0]); i++) {
        CHECK(hub_proto_page_key(continues[i]) == HUB_KEY_CONTINUE);
    }
    for (size_t i = 0; i < sizeof(alls) / sizeof(alls[0]); i++) {
        CHECK(hub_proto_page_key(alls[i]) == HUB_KEY_ALL);
    }
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        CHECK(hub_proto_page_key(others[i]) == HUB_KEY_NONE);
    }
}

static void test_blank(void)
{
    CHECK(hub_proto_is_blank(""));
    CHECK(hub_proto_is_blank(" \t\r\n　"));
    CHECK(!hub_proto_is_blank("　あ"));
    CHECK(hub_proto_blank_len("\n　 あ") == 5);
    CHECK(hub_proto_blank_len("あ") == 0);
    CHECK(hub_proto_unblank_len("あ\r\n　", 8) == 3);
    CHECK(hub_proto_unblank_len("　", 3) == 0);
}

static void test_completion_json(void)
{
    char *s = hub_proto_completion_json("返事", "openclaw", 123, "chatcmpl-x");
    CHECK(s != NULL);
    cJSON *j = cJSON_Parse(s);
    CHECK(j != NULL);
    CHECK(strcmp(cJSON_GetObjectItem(j, "object")->valuestring, "chat.completion") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(j, "model")->valuestring, "openclaw") == 0);
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0);
    cJSON *msg = cJSON_GetObjectItem(choice, "message");
    CHECK(strcmp(cJSON_GetObjectItem(msg, "content")->valuestring, "返事") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(choice, "finish_reason")->valuestring, "stop") == 0);
    cJSON_Delete(j);
    hub_proto_free(s);
}

static void test_completion_sse(void)
{
    char *s = hub_proto_completion_sse("返事", "muse", 123, "chatcmpl-x");
    CHECK(s != NULL);
    CHECK(strncmp(s, "data: {", 7) == 0);
    CHECK(strstr(s, "\"content\":\"返事\"") != NULL);
    CHECK(strstr(s, "\"finish_reason\":\"stop\"") != NULL);
    size_t n = strlen(s);
    CHECK(n > 16 && strcmp(s + n - 14, "data: [DONE]\n\n") == 0);
    hub_proto_free(s);
}

/* The widgets seen on the device, as the SDK forwards them (the HTML dropped). */
static const char WIDGET_LIST[] =
    "{\"id\":\"widget-0ed9\",\"kind\":\"generic_list\","
    "\"display_text\":\"**リスト型の見本：ウィジェットの種類一覧**\\n- 選択肢型\\n- 地図型\\n- リスト型\","
    "\"data\":{\"title\":\"リスト型の見本：ウィジェットの種類一覧\",\"items\":["
    "{\"title\":\"選択肢型\",\"subtitle\":\"タップで返信になるボタン\",\"tertiary_title\":\"任意\","
    "\"type\":\"generic\",\"data\":{},\"icons\":[]},"
    "{\"title\":\"地図型\",\"subtitle\":\"\",\"type\":\"generic\",\"data\":{},\"icons\":[]},"
    "{\"title\":\"リスト型\",\"subtitle\":\"項目の一覧\",\"type\":\"generic\",\"data\":{},\"icons\":[]}]}}";
static const char WIDGET_LIST_TEXT[] =
    "リスト型の見本：ウィジェットの種類一覧\n"
    "・選択肢型 — タップで返信になるボタン (任意)\n"
    "・地図型\n"
    "・リスト型 — 項目の一覧";
static const char WIDGET_MAP[] =
    "{\"id\":\"widget-e172\",\"kind\":\"local_map\",\"display_text\":\"地図型の見本：アイーナとパシフィコ横浜\","
    "\"data\":{\"typed_data\":{\"kind\":\"local_map\",\"elements\":["
    "{\"kind\":\"marker\",\"title\":\"アイーナ（NTいわて会場）\",\"subtitle\":\"明日11/8 12:00〜16:00\","
    "\"coordinate\":{\"latitude\":39.7017,\"longitude\":141.1365}},"
    "{\"kind\":\"route\",\"title\":\"経路\"},"
    "{\"kind\":\"marker\",\"title\":\"パシフィコ横浜\",\"subtitle\":\"11/9 10:00〜17:00\","
    "\"coordinate\":{\"latitude\":35.4575,\"longitude\":139.6375}}]}}}";
static const char WIDGET_MAP_TEXT[] =
    "地図型の見本：アイーナとパシフィコ横浜\n"
    "・アイーナ（NTいわて会場） — 明日11/8 12:00〜16:00\n"
    "・パシフィコ横浜 — 11/9 10:00〜17:00";
static const char WIDGET_HTML[] =
    "{\"id\":\"widget-3572\",\"kind\":\"html\",\"fallback_text\":\"サイコロ見本ウィジェット\","
    "\"data\":{\"fallback_text\":\"サイコロ見本ウィジェット\"}}";
static const char WIDGET_IMAGES[] =
    "{\"id\":\"widget-ce86\",\"kind\":\"image_options\",\"display_text\":\"Choose from ⁨4⁩ image options\","
    "\"data\":{\"images\":[{\"path\":\"a.png\"},{\"path\":\"b.png\"},{\"path\":\"c.png\"},{\"path\":\"d.png\"}],"
    "\"multi_select\":false}}";

static bool rendered_as(const char *json, const char *want)
{
    char out[1024];
    hub_proto_render_presentation(json, out, sizeof(out));
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "rendered: [%s]\n", out);
        return false;
    }
    return true;
}

static void test_render_list_and_map(void)
{
    CHECK(rendered_as(WIDGET_LIST, WIDGET_LIST_TEXT));
    CHECK(rendered_as(WIDGET_MAP, WIDGET_MAP_TEXT));
    /* no data.title: the first line of display_text; an item without a title is skipped */
    CHECK(rendered_as("{\"kind\":\"generic_list\",\"display_text\":\"**見出し**\\n- a\","
                      "\"data\":{\"items\":[{\"title\":\" a \\n b \"},{\"subtitle\":\"x\"},"
                      "{\"title\":\"c\",\"tertiary_title\":\"t\"}]}}",
                      "見出し\n・a b\n・c (t)"));
}

static void test_render_other_kinds_as_a_note(void)
{
    CHECK(rendered_as(WIDGET_HTML, "(Muse アプリに表示: サイコロ見本ウィジェット)"));
    CHECK(rendered_as(WIDGET_IMAGES, "(Muse アプリに表示: Choose from ⁨4⁩ image options)"));
    CHECK(rendered_as("{\"kind\":\"image_options\"}", "(Muse アプリに表示: 画像の選択肢)"));
    CHECK(rendered_as("{\"kind\":\"weather\",\"display_text\":\"**晴れ**\\n\\n 22℃ \"}",
                      "(Muse アプリに表示: 晴れ 22℃)"));
    CHECK(rendered_as("{\"kind\":\"generic_list\",\"data\":{\"items\":[]}}", "(Muse アプリに表示: ウィジェット)"));
    CHECK(rendered_as("not json", "(Muse アプリに表示: ウィジェット)"));
    CHECK(rendered_as("{\"kind\":\"html\"} x", "(Muse アプリに表示: ウィジェット)"));
    CHECK(rendered_as("", "(Muse アプリに表示: ウィジェット)"));
    CHECK(rendered_as("[1]", "(Muse アプリに表示: ウィジェット)"));
}

static void test_render_stays_in_the_buffer(void)
{
    char out[20];
    hub_proto_render_presentation(WIDGET_LIST, out, sizeof(out));
    CHECK(strlen(out) < sizeof(out));
    CHECK(strncmp(out, "リスト型", strlen(out) < 12 ? strlen(out) : 12) == 0);
    hub_proto_render_presentation(WIDGET_HTML, out, sizeof(out));
    CHECK(strlen(out) < sizeof(out));
}

static const char *lookup_widget_a(const char *id, void *ctx)
{
    (*(int *)ctx)++;
    return strcmp(id, "widget-A") == 0 ? WIDGET_HTML : NULL;
}

static void test_replace_widget_markers(void)
{
    char out[512];
    int calls = 0;
    hub_proto_replace_widget_markers("見本です。\n\n[[hatch_widget:widget-A]]\n[[hatch_widget:widget-B]]終わり",
                                     lookup_widget_a, &calls, out, sizeof(out));
    CHECK(strcmp(out, "見本です。\n\n(Muse アプリに表示: サイコロ見本ウィジェット)\n"
                      "(Muse アプリに表示: 選択肢など)終わり") == 0);
    CHECK(calls == 2);
    CHECK(strcmp(HUB_UNKNOWN_WIDGET, "(Muse アプリに表示: 選択肢など)") == 0);
    hub_proto_replace_widget_markers("a [[hatch_widget:]] b [[hatch_widget:widget-A", lookup_widget_a, &calls,
                                     out, sizeof(out));
    CHECK(strcmp(out, "a [[hatch_widget:]] b [[hatch_widget:widget-A") == 0);
    CHECK(calls == 2);
    hub_proto_replace_widget_markers("テキストだけ", lookup_widget_a, &calls, out, sizeof(out));
    CHECK(strcmp(out, "テキストだけ") == 0);
    char small[16];
    hub_proto_replace_widget_markers("あいう[[hatch_widget:widget-A]]", lookup_widget_a, &calls, small,
                                     sizeof(small));
    CHECK(strlen(small) < sizeof(small));
}

void run_turns_tests(void);
extern int turns_run;
extern int turns_failed;

int main(void)
{
    RUN(test_parse_takes_last_user_message);
    RUN(test_parse_joins_text_parts_and_reads_stream);
    RUN(test_parse_rejects_bad_requests);
    RUN(test_check_auth);
    RUN(test_utf8_helpers);
    RUN(test_clip);
    RUN(test_page_cut);
    RUN(test_page_key);
    RUN(test_blank);
    RUN(test_completion_json);
    RUN(test_completion_sse);
    RUN(test_render_list_and_map);
    RUN(test_render_other_kinds_as_a_note);
    RUN(test_render_stays_in_the_buffer);
    RUN(test_replace_widget_markers);
    run_turns_tests();
    s_run += turns_run;
    s_failed += turns_failed;
    printf("%d tests, %d failures\n", s_run, s_failed);
    return s_failed ? 1 : 0;
}
