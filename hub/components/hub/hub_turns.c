#include "hub_turns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub_proto.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HUB_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define HUB_PSRAM_BSS
#endif

#define MSG_MAX 8
#define MSG_BYTES 1024
#define ERROR_MAX 160

const char HUB_STILL_WORKING[] = "まだ考え中です。少ししてからもう一度話しかけてください。";
const char HUB_CONTINUE_NOTE[] = "(まだ考え中です。もう一度話しかけると続きを返します)";
const char HUB_MSG_BUSY[] = "StickS3 で音声の会話中です。終わってからもう一度どうぞ。";
const char HUB_MSG_NOT_SET_UP[] = "Muse が未設定です。ペアリングを確認してください。";
const char HUB_MSG_INTERNAL[] = "Muse に届きませんでした (内部エラー)。";

typedef struct {
    char text[MSG_BYTES];
    bool used;
} msg_t;

static const hub_turns_ops_t *s_ops;
static int s_wait_ms;
static int64_t s_hold_ms;
static bool s_running;
static bool s_finished;
static bool s_detached;            /* the request stopped waiting: hold the result */
static HUB_PSRAM_BSS msg_t s_msgs[MSG_MAX];
static char s_error[ERROR_MAX];    /* non-empty: the turn failed */
static HUB_PSRAM_BSS char s_held[HUB_REPLY_MAX];
static bool s_has_held;
static int64_t s_held_at;

void hub_turns_init(const hub_turns_ops_t *ops, const int wait_ms, const int64_t hold_ms)
{
    s_ops = ops;
    s_wait_ms = wait_ms;
    s_hold_ms = hold_ms;
    s_running = s_finished = s_detached = s_has_held = false;
    s_error[0] = '\0';
    memset(s_msgs, 0, sizeof(s_msgs));
}

static void append(char *dst, const size_t cap, const char *src)
{
    const size_t used = strlen(dst);
    if (used + 1 < cap) {
        snprintf(dst + used, cap - used, "%s", src);
        hub_proto_utf8_trim(dst);
    }
}

static int msg_index(const char *fields)
{
    const char *p = fields ? strstr(fields, "\"msg\":") : NULL;
    const int i = p ? atoi(p + 6) : 0;
    return i >= 0 && i < MSG_MAX ? i : -1;
}

/* The reply so far: the messages joined by blank lines. Call with the lock held. */
static void build(char *out, const size_t cap)
{
    out[0] = '\0';
    for (int i = 0; i < MSG_MAX; i++) {
        if (!s_msgs[i].used || !s_msgs[i].text[0]) {
            continue;
        }
        if (out[0]) {
            append(out, cap, "\n\n");
        }
        append(out, cap, s_msgs[i].text);
    }
}

static void result(char *out, const size_t cap)
{
    if (s_error[0]) {
        snprintf(out, cap, "%s", s_error);
    } else {
        build(out, cap);
    }
}

static void set_error(const char *text)
{
    if (text && strstr(text, "BUSY WITH A VOICE TURN")) {
        snprintf(s_error, sizeof(s_error), "%s", HUB_MSG_BUSY);
    } else if (text && strstr(text, "MUSE NOT SET UP")) {
        snprintf(s_error, sizeof(s_error), "%s", HUB_MSG_NOT_SET_UP);
    } else {
        snprintf(s_error, sizeof(s_error), "Muse に届きませんでした (%.80s)。", text ? text : "error");
        hub_proto_utf8_trim(s_error);
    }
}

void hub_turns_on_event(const char *type, const char *fields, const char *text)
{
    s_ops->lock();
    if (!s_running) {
        s_ops->unlock();
        return;                    /* not ours: a typed turn from the USB console */
    }
    const bool is_final = strcmp(type, "final") == 0;
    if (is_final || strcmp(type, "text") == 0) {
        const int i = msg_index(fields);
        if (i >= 0 && text) {
            if (is_final) {
                s_msgs[i].text[0] = '\0';
            }
            s_msgs[i].used = true;
            append(s_msgs[i].text, MSG_BYTES, text);
        }
    } else if (strcmp(type, "done") == 0 || strcmp(type, "error") == 0) {
        if (type[0] == 'e') {
            set_error(text);
        }
        s_running = false;
        s_finished = true;
        if (s_detached) {
            result(s_held, sizeof(s_held));
            s_has_held = true;
            s_held_at = s_ops->now_ms();
        }
        s_ops->signal_done();
    }
    s_ops->unlock();
}

bool hub_turns_busy(void)
{
    s_ops->lock();
    const bool busy = s_running;
    s_ops->unlock();
    return busy;
}

/* partial clipped so that "partial\nNOTE" fits the card. */
static void with_note(const char *partial, char *out, const size_t out_len)
{
    if (!partial[0]) {
        snprintf(out, out_len, "%s", HUB_CONTINUE_NOTE);
        return;
    }
    const size_t room = HUB_MAX_CHARS - hub_proto_utf8_chars(HUB_CONTINUE_NOTE) - 1;
    char *clipped = malloc(HUB_REPLY_MAX);
    if (!clipped) {
        snprintf(out, out_len, "%s", HUB_CONTINUE_NOTE);
        return;
    }
    hub_proto_clip(partial, clipped, HUB_REPLY_MAX, room);
    snprintf(out, out_len, "%s\n%s", clipped, HUB_CONTINUE_NOTE);
    free(clipped);
}

static void finish(char *out, const size_t out_len)
{
    char *copy = strdup(out);
    if (copy) {
        hub_proto_clip(copy, out, out_len, HUB_MAX_CHARS);
        free(copy);
    }
}

void hub_turns_ask(const char *prompt, char *out, const size_t out_len)
{
    s_ops->lock();
    if (s_has_held) {
        s_has_held = false;
        if (s_ops->now_ms() - s_held_at <= s_hold_ms) {
            snprintf(out, out_len, "%s", s_held);
            s_ops->unlock();
            finish(out, out_len);
            return;
        }
    }
    if (s_running) {
        snprintf(out, out_len, "%s", HUB_STILL_WORKING);
        s_ops->unlock();
        return;
    }
    memset(s_msgs, 0, sizeof(s_msgs));
    s_error[0] = '\0';
    s_finished = s_detached = false;
    s_running = true;
    s_ops->reset_done();
    s_ops->unlock();

    if (!s_ops->start_turn(prompt)) {
        s_ops->lock();
        s_running = false;
        s_ops->unlock();
        snprintf(out, out_len, "%s", HUB_MSG_INTERNAL);
        return;
    }
    s_ops->wait_done(s_wait_ms);

    s_ops->lock();
    if (s_finished) {
        result(out, out_len);
        s_ops->unlock();
        finish(out, out_len);
        return;
    }
    s_detached = true;
    char *partial = malloc(HUB_REPLY_MAX);
    if (partial) {
        build(partial, HUB_REPLY_MAX);
    }
    s_ops->unlock();
    with_note(partial ? partial : "", out, out_len);
    free(partial);
}
