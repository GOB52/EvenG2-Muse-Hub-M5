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

int turns_failed;
int turns_run;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_done_cv = PTHREAD_COND_INITIALIZER;
static bool s_done;
static int64_t s_now;
static int s_prompt_count;
static const char *s_script;   /* what start_turn emits: fast multi busy slow longslow none fail */

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
    (void)prompt;
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
    }
    return true;                    /* "none": nothing arrives before the wait ends */
}

static const hub_turns_ops_t OPS = {
    .now_ms = now_ms, .lock = lock, .unlock = unlock, .reset_done = reset_done,
    .wait_done = wait_done, .signal_done = signal_done, .start_turn = start_turn,
};

static void setup(const char *script)
{
    s_script = script;
    s_prompt_count = 0;
    s_now = 1000;
    hub_turns_init(&OPS, 100, 600000, TURN_CAP);
    turns_run++;
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
    setup("longslow");
    hub_turns_ask("q", out, sizeof(out));
    const size_t n = strlen(out);
    const size_t note = strlen(HUB_CONTINUE_NOTE);
    CHECK(n > note && strcmp(out + n - note, HUB_CONTINUE_NOTE) == 0);
    CHECK(hub_proto_utf8_chars(out) <= HUB_MAX_CHARS);
}

static void test_long_held_reply_is_clipped(void)
{
    char out[HUB_REPLY_MAX];
    char piece[1000];
    setup("none");
    hub_turns_ask("q", out, sizeof(out));
    hub_turns_on_event("sent", "\"bytes\":1", NULL);
    repeat(piece, sizeof(piece), "あ", 300);
    hub_turns_on_event("text", "\"msg\":0", piece);
    hub_turns_on_event("text", "\"msg\":1", piece);
    hub_turns_on_event("done", "", NULL);
    hub_turns_ask("q2", out, sizeof(out));
    CHECK(hub_proto_utf8_chars(out) == HUB_MAX_CHARS);
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
    test_long_held_reply_is_clipped();
    test_foreign_lines_before_sent_are_ignored();
    test_busy_before_sent_is_still_mapped();
    test_cant_reach_before_sent_ends_the_turn();
    test_stuck_turn_is_dropped_after_the_cap();
}
