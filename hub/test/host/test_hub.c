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
    run_turns_tests();
    s_run += turns_run;
    s_failed += turns_failed;
    printf("%d tests, %d failures\n", s_run, s_failed);
    return s_failed ? 1 : 0;
}
