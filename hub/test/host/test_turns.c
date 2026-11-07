#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hub_proto.h"
#include "hub_turns.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            turns_failed++;                                                  \
        }                                                                    \
    } while (0)

#define TURN_CAP 960000
#define MAX_CHARS 2000
#define CARD_CHARS 400
#define ALL_MAX_CHARS 5000

int turns_failed;
int turns_run;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_done_cv = PTHREAD_COND_INITIALIZER;
static bool s_done;
static int64_t s_now;
static int s_prompt_count;
static const char *s_script;   /* what start_turn emits: fast multi busy slow longslow longfast reply none fail */
static const char *s_reply;    /* the "reply" script's one message; NULL sends no text */
static char s_last_prompt[256];

static int64_t now_ms(void) { return s_now; }
static void lock(void) { pthread_mutex_lock(&s_mu); }
static void unlock(void) { pthread_mutex_unlock(&s_mu); }

static void reset_done(void)
{
    pthread_mutex_lock(&s_done_mu);
    s_done = false;
    pthread_mutex_unlock(&s_done_mu);
}

static bool wait_done(const int timeout_ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_done_mu);
    while (!s_done && pthread_cond_timedwait(&s_done_cv, &s_done_mu, &until) == 0) {
    }
    const bool done = s_done;
    pthread_mutex_unlock(&s_done_mu);
    return done;
}

static void signal_done(void)
{
    pthread_mutex_lock(&s_done_mu);
    s_done = true;
    pthread_cond_signal(&s_done_cv);
    pthread_mutex_unlock(&s_done_mu);
}

static void repeat(char *out, const size_t cap, const char *unit, const int times)
{
    out[0] = '\0';
    for (int i = 0; i < times && strlen(out) + strlen(unit) < cap; i++) {
        strcat(out, unit);
    }
}

static bool start_turn(const char *prompt)
{
    snprintf(s_last_prompt, sizeof(s_last_prompt), "%s", prompt);
    s_prompt_count++;
    char piece[1600];
    if (strcmp(s_script, "fail") == 0) {
        return false;
    }
    if (strcmp(s_script, "fast") == 0) {
        hub_turns_on_event("sent", "\"bytes\":3", NULL);
        hub_turns_on_event("text", "\"msg\":0", "はい、");
        hub_turns_on_event("text", "\"msg\":0", "送るよ。");
        hub_turns_on_event("done", "\"messages\":1,\"complete\":true", NULL);
    } else if (strcmp(s_script, "multi") == 0) {
        hub_turns_on_event("sent", "\"bytes\":1", NULL);
        hub_turns_on_event("text", "\"msg\":0", "途中");
        hub_turns_on_event("final", "\"msg\":0", "一つ目");
        hub_turns_on_event("text", "\"msg\":1", "二つ目");
        hub_turns_on_event("done", "\"messages\":2,\"complete\":true", NULL);
    } else if (strcmp(s_script, "busy") == 0) {
        hub_turns_on_event("error", "", "BUSY WITH A VOICE TURN");
    } else if (strcmp(s_script, "slow") == 0) {
        hub_turns_on_event("sent", "\"bytes\":2", NULL);
        hub_turns_on_event("text", "\"msg\":0", "途中まで。");
    } else if (strcmp(s_script, "foreign") == 0) {
        hub_turns_on_event("error", "", "INTERRUPTED");
        hub_turns_on_event("busy", "\"on\":true", NULL);
        hub_turns_on_event("text", "\"msg\":0", "他人の");
        hub_turns_on_event("final", "\"msg\":0", "他人の最終");
        hub_turns_on_event("done", "\"messages\":1,\"complete\":true", NULL);
        hub_turns_on_event("sent", "\"bytes\":1", NULL);
        hub_turns_on_event("text", "\"msg\":0", "自分の返事");
        hub_turns_on_event("done", "\"messages\":1,\"complete\":true", NULL);
    } else if (strcmp(s_script, "unreachable") == 0) {
        hub_turns_on_event("error", "", "CAN'T REACH MUSE");
    } else if (strcmp(s_script, "longslow") == 0) {
        hub_turns_on_event("sent", "\"bytes\":3", NULL);
        repeat(piece, sizeof(piece), "a", 500);
        hub_turns_on_event("text", "\"msg\":0", piece);
    } else if (strcmp(s_script, "reply") == 0) {
        hub_turns_on_event("sent", "\"bytes\":3", NULL);
        if (s_reply) {
            hub_turns_on_event("text", "\"msg\":0", s_reply);
        }
        hub_turns_on_event("done", "\"messages\":1,\"complete\":true", NULL);
    } else if (strcmp(s_script, "longfast") == 0) {
        hub_turns_on_event("sent", "\"bytes\":3", NULL);
        repeat(piece, sizeof(piece), "あ", 500);
        hub_turns_on_event("text", "\"msg\":0", piece);
        hub_turns_on_event("done", "\"messages\":1,\"complete\":true", NULL);
    }
    return true;                    /* "none": nothing arrives before the wait ends */
}

static const hub_turns_ops_t OPS = {
    .now_ms = now_ms, .lock = lock, .unlock = unlock, .reset_done = reset_done,
    .wait_done = wait_done, .signal_done = signal_done, .start_turn = start_turn,
};

static void setup_pages(const char *script, const size_t max_chars, const size_t all_max_chars)
{
    s_script = script;
    s_reply = NULL;
    s_prompt_count = 0;
    s_last_prompt[0] = '\0';
    s_now = 1000;
    hub_turns_init(&OPS, 100, 600000, TURN_CAP, max_chars, all_max_chars);
    turns_run++;
}

static void setup_chars(const char *script, const size_t max_chars)
{
    setup_pages(script, max_chars, ALL_MAX_CHARS);
}

static void setup(const char *script)
{
    setup_chars(script, MAX_CHARS);
}

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        const size_t n = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 0;
        if (n == 0) {
            return false;
        }
        for (size_t i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return false;          /* also stops at the terminator */
            }
        }
        p += n;
    }
    return true;
}

static bool ends_with_note(const char *s)
{
    const size_t n = strlen(s);
    const size_t note = strlen(HUB_PAGE_NOTE);
    return n > note && strcmp(s + n - note, HUB_PAGE_NOTE) == 0 && s[n - note - 1] == '\n';
}

/* Appends page to text without its "\nNOTE". */
static void unpage(char *text, const size_t cap, const char *page)
{
    const size_t n = ends_with_note(page) ? strlen(page) - strlen(HUB_PAGE_NOTE) - 1 : strlen(page);
    const size_t used = strlen(text);
    if (used + n < cap) {
        memcpy(text + used, page, n);
        text[used + n] = '\0';
    }
}

static void setup_reply(const char *reply, const size_t max_chars)
{
    setup_chars("reply", max_chars);
    s_reply = reply;
}

static void test_fast_reply_is_returned_whole(void)
{
    char out[HUB_REPLY_MAX];
    setup("fast");
    hub_turns_ask("送って", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
    CHECK(!hub_turns_busy());
}

static void test_final_replaces_and_messages_are_joined(void)
{
    char out[HUB_REPLY_MAX];
    setup("multi");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, "一つ目\n\n二つ目") == 0);
}

static void test_voice_turn_busy_is_mapped(void)
{
    char out[HUB_REPLY_MAX];
    setup("busy");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, HUB_MSG_BUSY) == 0);
    CHECK(!hub_turns_busy());
}

static void test_start_failure_is_internal_error(void)
{
    char out[HUB_REPLY_MAX];
    setup("fail");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, HUB_MSG_INTERNAL) == 0);
    CHECK(!hub_turns_busy());
}

static void test_slow_reply_is_partial_then_held(void)
{
    char out[HUB_REPLY_MAX];
    char want[HUB_REPLY_MAX];
    setup("slow");
    hub_turns_ask("q1", out, sizeof(out));
    snprintf(want, sizeof(want), "途中まで。\n%s", HUB_CONTINUE_NOTE);
    CHECK(strcmp(out, want) == 0);
    CHECK(hub_turns_busy());

    hub_turns_ask("q2", out, sizeof(out));
    CHECK(strcmp(out, HUB_STILL_WORKING) == 0);

    hub_turns_on_event("text", "\"msg\":0", "続き。");
    hub_turns_on_event("done", "\"messages\":1,\"complete\":true", NULL);
    CHECK(!hub_turns_busy());

    s_script = "fast";
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(strcmp(out, "途中まで。続き。") == 0);
    CHECK(s_prompt_count == 1);       /* the held reply was returned; nothing was sent */

    hub_turns_ask("次", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
}

static void test_held_reply_expires(void)
{
    char out[HUB_REPLY_MAX];
    setup("slow");
    hub_turns_ask("q1", out, sizeof(out));
    hub_turns_on_event("done", "", NULL);
    s_now += 600001;
    s_script = "fast";
    hub_turns_ask("q2", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
    CHECK(s_prompt_count == 2);
}

static void test_events_without_a_turn_are_ignored(void)
{
    char out[HUB_REPLY_MAX];
    setup("fast");
    hub_turns_on_event("text", "\"msg\":0", "USB の chat.py のターン");
    hub_turns_on_event("done", "", NULL);
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
}

static void test_no_partial_is_the_note_only(void)
{
    char out[HUB_REPLY_MAX];
    setup("none");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, HUB_CONTINUE_NOTE) == 0);
}

static void test_long_partial_keeps_the_note(void)
{
    char out[HUB_REPLY_MAX];
    setup_chars("longslow", CARD_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    const size_t n = strlen(out);
    const size_t note = strlen(HUB_CONTINUE_NOTE);
    CHECK(n > note && strcmp(out + n - note, HUB_CONTINUE_NOTE) == 0);
    CHECK(hub_proto_utf8_chars(out) <= CARD_CHARS);
}

static void test_long_held_reply_is_paged(void)
{
    char out[HUB_REPLY_MAX];
    char piece[2000];
    char want[2000];
    setup_chars("none", CARD_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    hub_turns_on_event("sent", "\"bytes\":1", NULL);
    repeat(piece, sizeof(piece), "あ", 600);
    hub_turns_on_event("text", "\"msg\":0", piece);
    hub_turns_on_event("done", "", NULL);
    hub_turns_ask("続き", out, sizeof(out));
    repeat(want, sizeof(want), "あ", CARD_CHARS - (int)hub_proto_utf8_chars(HUB_PAGE_NOTE) - 1);
    strcat(want, "\n");
    strcat(want, HUB_PAGE_NOTE);
    CHECK(strcmp(out, want) == 0);
    CHECK(hub_proto_utf8_chars(out) == CARD_CHARS);
    hub_turns_ask("続き", out, sizeof(out));
    repeat(want, sizeof(want), "あ", 600 - (CARD_CHARS - (int)hub_proto_utf8_chars(HUB_PAGE_NOTE) - 1));
    CHECK(strcmp(out, want) == 0);
    CHECK(s_prompt_count == 1);
}

/* Regression: a single message was cut at 1023 bytes (341 Japanese characters). */
static void test_long_message_is_returned_whole(void)
{
    char out[HUB_REPLY_MAX];
    char want[1600];
    setup("longfast");
    hub_turns_ask("q", out, sizeof(out));
    repeat(want, sizeof(want), "あ", 500);
    CHECK(strlen(want) == 1500);
    CHECK(strcmp(out, want) == 0);
}

static void test_reply_is_capped_at_reply_max_bytes(void)
{
    char out[HUB_REPLY_MAX];
    char piece[400];
    setup_chars("none", HUB_REPLY_MAX);   /* no character clip: only the byte cap applies */
    hub_turns_ask("q", out, sizeof(out));
    hub_turns_on_event("sent", "\"bytes\":1", NULL);
    repeat(piece, sizeof(piece), "あ", 100);
    for (int i = 0; i < 80; i++) {
        char fields[16];
        snprintf(fields, sizeof(fields), "\"msg\":%d", i % 8);
        hub_turns_on_event("text", fields, piece);
    }
    hub_turns_on_event("done", "", NULL);
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(strlen(out) < HUB_REPLY_MAX);
    CHECK(strlen(out) > HUB_REPLY_MAX - 64);
    CHECK(valid_utf8(out));
}

static void test_foreign_lines_before_sent_are_ignored(void)
{
    char out[HUB_REPLY_MAX];
    setup("foreign");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, "自分の返事") == 0);
    CHECK(!hub_turns_busy());
}

static void test_busy_before_sent_is_still_mapped(void)
{
    char out[HUB_REPLY_MAX];
    setup("busy");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, HUB_MSG_BUSY) == 0);
}

static void test_cant_reach_before_sent_ends_the_turn(void)
{
    char out[HUB_REPLY_MAX];
    setup("unreachable");
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, "Muse に届きませんでした (CAN'T REACH MUSE)。") == 0);
    CHECK(!hub_turns_busy());
}

static void test_stuck_turn_is_dropped_after_the_cap(void)
{
    char out[HUB_REPLY_MAX];
    setup("slow");
    hub_turns_ask("q1", out, sizeof(out));
    CHECK(hub_turns_busy());
    s_now += TURN_CAP - 1;
    hub_turns_ask("q2", out, sizeof(out));
    CHECK(strcmp(out, HUB_STILL_WORKING) == 0);
    s_now += 2;
    s_script = "fast";
    hub_turns_ask("q3", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
    CHECK(s_prompt_count == 2);
}

#define PAGE_CHARS 300

static void test_long_reply_is_paged(void)
{
    char text[2400];
    char out[HUB_REPLY_MAX];
    char joined[2400] = "";
    repeat(text, sizeof(text), "あ", 700);
    setup_reply(text, PAGE_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    CHECK(ends_with_note(out));
    CHECK(hub_proto_utf8_chars(out) <= PAGE_CHARS);
    unpage(joined, sizeof(joined), out);
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(ends_with_note(out));
    CHECK(hub_proto_utf8_chars(out) <= PAGE_CHARS);
    unpage(joined, sizeof(joined), out);
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(!ends_with_note(out));
    CHECK(strstr(out, HUB_PAGE_NOTE) == NULL);
    unpage(joined, sizeof(joined), out);
    CHECK(strcmp(joined, text) == 0);
    CHECK(s_prompt_count == 1);
    s_script = "fast";
    hub_turns_ask("次", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
    CHECK(s_prompt_count == 2);
}

static void test_page_split_prefers_a_sentence_end(void)
{
    char text[2400];
    char tail[1200];
    char want[1200];
    char out[HUB_REPLY_MAX];
    repeat(text, sizeof(text), "あ", 250);
    strcat(text, "。");
    repeat(tail, sizeof(tail), "い", 200);
    strcat(text, tail);
    setup_reply(text, PAGE_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    repeat(want, sizeof(want), "あ", 250);
    strcat(want, "。\n");
    strcat(want, HUB_PAGE_NOTE);
    CHECK(strcmp(out, want) == 0);
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(strcmp(out, tail) == 0);
}

static void test_page_split_ignores_an_early_sentence_end(void)
{
    char text[2400];
    char tail[1200];
    char out[HUB_REPLY_MAX];
    repeat(text, sizeof(text), "あ", 100);
    strcat(text, "。");
    repeat(tail, sizeof(tail), "い", 300);
    strcat(text, tail);
    setup_reply(text, PAGE_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    CHECK(ends_with_note(out));
    CHECK(hub_proto_utf8_chars(out) == PAGE_CHARS);
}

static void test_page_split_strips_newlines_at_the_cut(void)
{
    char text[2400];
    char tail[1200];
    char want[1200];
    char out[HUB_REPLY_MAX];
    repeat(text, sizeof(text), "あ", 250);
    strcat(text, "\n\n");
    repeat(tail, sizeof(tail), "い", 200);
    strcat(text, tail);
    setup_reply(text, PAGE_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    repeat(want, sizeof(want), "あ", 250);
    CHECK(strncmp(out, want, strlen(want)) == 0);
    CHECK(ends_with_note(out));
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(strcmp(out, tail) == 0);
}

static void test_held_page_expires(void)
{
    char text[2400];
    char out[HUB_REPLY_MAX];
    repeat(text, sizeof(text), "あ", 700);
    setup_reply(text, PAGE_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    CHECK(ends_with_note(out));
    s_now += 600001;
    s_script = "fast";
    hub_turns_ask("q2", out, sizeof(out));
    CHECK(strcmp(out, "はい、送るよ。") == 0);
    CHECK(s_prompt_count == 2);
}

static void test_empty_reply_gives_the_note(void)
{
    char out[HUB_REPLY_MAX];
    setup_reply(NULL, MAX_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, HUB_EMPTY_NOTE) == 0);
    setup_reply("\n\n", MAX_CHARS);
    hub_turns_ask("q", out, sizeof(out));
    CHECK(strcmp(out, HUB_EMPTY_NOTE) == 0);
    setup("none");                    /* held after a timeout */
    hub_turns_ask("q", out, sizeof(out));
    hub_turns_on_event("sent", "\"bytes\":1", NULL);
    hub_turns_on_event("done", "", NULL);
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(strcmp(out, HUB_EMPTY_NOTE) == 0);
}

#define KEY_ALL_MAX 2000

/* A 1500-character reply paged at PAGE_CHARS, after its first page. */
static void ask_first_page(char *text, const size_t text_cap, const size_t all_max_chars, char *out,
                           const size_t out_len)
{
    repeat(text, text_cap, "あ", 1500);
    setup_pages("reply", PAGE_CHARS, all_max_chars);
    s_reply = text;
    hub_turns_ask("q", out, out_len);
}

static int page_room(const size_t max_chars)
{
    return (int)max_chars - (int)hub_proto_utf8_chars(HUB_PAGE_NOTE) - 1;
}

static void test_continue_words_give_the_next_page(void)
{
    static const char *const prompts[] = {"続き", "続きを見せて。", " つ づ き ！", "「次」", "もっと…"};
    char text[4800];
    char want[4800];
    char out[HUB_REPLY_MAX];
    for (size_t i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++) {
        ask_first_page(text, sizeof(text), KEY_ALL_MAX, out, sizeof(out));
        hub_turns_ask(prompts[i], out, sizeof(out));
        repeat(want, sizeof(want), "あ", page_room(PAGE_CHARS));
        strcat(want, "\n");
        strcat(want, HUB_PAGE_NOTE);
        CHECK(strcmp(out, want) == 0);
        CHECK(s_prompt_count == 1);
    }
}

static void test_all_words_give_the_whole_rest(void)
{
    static const char *const prompts[] = {"終わりまで", "最後まで。", "「全部」！", "全文", "続きを全部"};
    char text[4800];
    char want[4800];
    char out[HUB_REPLY_MAX];
    for (size_t i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++) {
        ask_first_page(text, sizeof(text), KEY_ALL_MAX, out, sizeof(out));
        hub_turns_ask(prompts[i], out, sizeof(out));
        repeat(want, sizeof(want), "あ", 1500 - page_room(PAGE_CHARS));
        CHECK(strcmp(out, want) == 0);
        CHECK(s_prompt_count == 1);
        s_script = "fast";
        hub_turns_ask("続き", out, sizeof(out));   /* nothing is held any more */
        CHECK(strcmp(out, "はい、送るよ。") == 0);
    }
}

static void test_all_pages_a_rest_longer_than_all_max(void)
{
    char text[4800];
    char want[4800];
    char out[HUB_REPLY_MAX];
    ask_first_page(text, sizeof(text), 500, out, sizeof(out));
    hub_turns_ask("全部", out, sizeof(out));
    repeat(want, sizeof(want), "あ", page_room(500));
    strcat(want, "\n");
    strcat(want, HUB_PAGE_NOTE);
    CHECK(strcmp(out, want) == 0);
    hub_turns_ask("続き", out, sizeof(out));
    CHECK(ends_with_note(out) && hub_proto_utf8_chars(out) <= PAGE_CHARS);
    CHECK(s_prompt_count == 1);
}

static void test_new_question_drops_the_held_pages(void)
{
    static const char *const prompts[] = {"明日の天気は", "次の東京の天気と明日の大阪の天気を教えてください"};
    char text[4800];
    char out[HUB_REPLY_MAX];
    for (size_t i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++) {
        ask_first_page(text, sizeof(text), KEY_ALL_MAX, out, sizeof(out));
        s_script = "fast";
        hub_turns_ask(prompts[i], out, sizeof(out));
        CHECK(strcmp(out, "はい、送るよ。") == 0);
        CHECK(s_prompt_count == 2);
        CHECK(strcmp(s_last_prompt, prompts[i]) == 0);
        hub_turns_ask("続き", out, sizeof(out));   /* the old pages are gone */
        CHECK(strcmp(out, "はい、送るよ。") == 0);
        CHECK(s_prompt_count == 3);
    }
}

void run_turns_tests(void)
{
    test_fast_reply_is_returned_whole();
    test_final_replaces_and_messages_are_joined();
    test_voice_turn_busy_is_mapped();
    test_start_failure_is_internal_error();
    test_slow_reply_is_partial_then_held();
    test_held_reply_expires();
    test_events_without_a_turn_are_ignored();
    test_no_partial_is_the_note_only();
    test_long_partial_keeps_the_note();
    test_long_held_reply_is_paged();
    test_long_reply_is_paged();
    test_page_split_prefers_a_sentence_end();
    test_page_split_ignores_an_early_sentence_end();
    test_page_split_strips_newlines_at_the_cut();
    test_held_page_expires();
    test_empty_reply_gives_the_note();
    test_continue_words_give_the_next_page();
    test_all_words_give_the_whole_rest();
    test_all_pages_a_rest_longer_than_all_max();
    test_new_question_drops_the_held_pages();
    test_long_message_is_returned_whole();
    test_reply_is_capped_at_reply_max_bytes();
    test_foreign_lines_before_sent_are_ignored();
    test_busy_before_sent_is_still_mapped();
    test_cant_reach_before_sent_ends_the_turn();
    test_stuck_turn_is_dropped_after_the_cap();
}
