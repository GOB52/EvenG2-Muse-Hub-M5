#include "hub_turns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub_mem.h"
#include "hub_proto.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HUB_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define HUB_PSRAM_BSS
#endif

#define MSG_MAX 8
#define MSG_MIN_CAP 256            /* first allocation of a message's text; doubled as it grows */
#define REPLY_BYTES (HUB_REPLY_MAX - 1)
#define ERROR_MAX 160

const char HUB_STILL_WORKING[] = "まだ考え中です。少ししてからもう一度話しかけてください。";
const char HUB_CONTINUE_NOTE[] = "(まだ考え中です。もう一度話しかけると続きを返します)";
const char HUB_PAGE_NOTE[] = "(続きは「続き」、残り全部は「終わりまで」と話しかけてください)";
const char HUB_EMPTY_NOTE[] = "(Muse の返事にテキストがありませんでした。リンクや画像はガジェットに届きません)";
const char HUB_MSG_BUSY[] = "StickS3 で音声の会話中です。終わってからもう一度どうぞ。";
const char HUB_MSG_NOT_SET_UP[] = "Muse が未設定です。ペアリングを確認してください。";
const char HUB_MSG_INTERNAL[] = "Muse に届きませんでした (内部エラー)。";

typedef struct {
    char *text;                    /* hub_mem; NULL until the first text */
    size_t len;
    size_t cap;
    bool used;
} msg_t;

static const hub_turns_ops_t *s_ops;
static int s_wait_ms;
static int64_t s_hold_ms;
static int64_t s_cap_ms;
static size_t s_max_chars;
static size_t s_all_max_chars;
static int64_t s_started_at;
static bool s_running;
static bool s_finished;
static bool s_sent;                /* Muse accepted this turn's text (the hub's own "sent" line) */
static bool s_detached;            /* the request stopped waiting: hold the result */
static HUB_PSRAM_BSS msg_t s_msgs[MSG_MAX];
static char s_error[ERROR_MAX];    /* non-empty: the turn failed */
static HUB_PSRAM_BSS char s_held[HUB_REPLY_MAX];
static bool s_has_held;
static int64_t s_held_at;

static void msgs_reset(void)
{
    for (int i = 0; i < MSG_MAX; i++) {
        hub_mem_free(s_msgs[i].text);
    }
    memset(s_msgs, 0, sizeof(s_msgs));
}

void hub_turns_init(const hub_turns_ops_t *ops, const int wait_ms, const int64_t hold_ms, const int64_t cap_ms,
                    const size_t max_chars, const size_t all_max_chars)
{

    s_ops = ops;
    s_wait_ms = wait_ms;
    s_hold_ms = hold_ms;
    s_cap_ms = cap_ms;
    s_max_chars = max_chars;
    s_all_max_chars = all_max_chars;
    s_running = s_finished = s_detached = s_has_held = s_sent = false;
    s_error[0] = '\0';
    msgs_reset();
}

static void append(char *dst, const size_t cap, const char *src)
{
    const size_t used = strlen(dst);
    if (used + 1 < cap) {
        snprintf(dst + used, cap - used, "%s", src);
        hub_proto_utf8_trim(dst);
    }
}

static size_t msgs_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < MSG_MAX; i++) {
        n += s_msgs[i].len;
    }
    return n;
}

static bool msg_reserve(msg_t *m, const size_t need)
{
    if (need <= m->cap) {
        return true;
    }
    size_t cap = m->cap ? m->cap : MSG_MIN_CAP;
    while (cap < need) {
        cap *= 2;
    }
    char *text = hub_mem_realloc(m->text, cap);
    if (!text) {
        return false;
    }
    m->text = text;
    m->cap = cap;
    return true;
}

/* Appends src to m while all the messages together stay within REPLY_BYTES. Call with the lock held. */
static void msg_append(msg_t *m, const char *src)
{
    const size_t used = msgs_bytes();
    const size_t room = used < REPLY_BYTES ? REPLY_BYTES - used : 0;
    const size_t len = strlen(src);
    const size_t n = len < room ? len : room;
    if (n == 0 || !msg_reserve(m, m->len + n + 1)) {
        return;
    }
    memcpy(m->text + m->len, src, n);
    m->text[m->len + n] = '\0';
    hub_proto_utf8_trim(m->text);
    m->len = strlen(m->text);
}

static void msg_clear(msg_t *m)
{
    m->len = 0;
    if (m->text) {
        m->text[0] = '\0';
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
        if (!s_msgs[i].used || !s_msgs[i].len) {
            continue;
        }
        if (out[0]) {
            append(out, cap, "\n\n");
        }
        append(out, cap, s_msgs[i].text);
    }
}

static bool is_blank(const char *s)
{
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
            return false;
        }
    }
    return true;
}

/* The finished turn's reply. A reply without text (Muse sent only a link or a card) gets a note. */
static void result(char *out, const size_t cap)
{
    if (s_error[0]) {
        snprintf(out, cap, "%s", s_error);
        return;
    }
    build(out, cap);
    if (is_blank(out)) {
        snprintf(out, cap, "%s", HUB_EMPTY_NOTE);
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

/*
 * Before the hub's own "sent" line, the lines on the console belong to another
 * turn, except a refusal of this one (not set up, busy, can't reach Muse).
 * Call with the lock held.
 */
static bool accepts_before_sent(const char *type, const char *text)
{
    if (strcmp(type, "sent") == 0) {
        s_sent = true;
        return false;
    }
    return strcmp(type, "error") == 0 && !(text && strstr(text, "INTERRUPTED"));
}

void hub_turns_on_event(const char *type, const char *fields, const char *text)
{
    s_ops->lock();
    if (!s_running) {
        s_ops->unlock();
        return;                    /* not ours: a typed turn from the USB console */
    }
    if (!s_sent && !accepts_before_sent(type, text)) {
        s_ops->unlock();
        return;
    }
    const bool is_final = strcmp(type, "final") == 0;
    if (is_final || strcmp(type, "text") == 0) {
        const int i = msg_index(fields);
        if (i >= 0 && text) {
            if (is_final) {
                msg_clear(&s_msgs[i]);
            }
            s_msgs[i].used = true;
            msg_append(&s_msgs[i], text);
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
    const size_t room = s_max_chars - hub_proto_utf8_chars(HUB_CONTINUE_NOTE) - 1;
    char *clipped = hub_mem_alloc(HUB_REPLY_MAX);
    if (!clipped) {
        snprintf(out, out_len, "%s", HUB_CONTINUE_NOTE);
        return;
    }
    hub_proto_clip(partial, clipped, HUB_REPLY_MAX, room);
    snprintf(out, out_len, "%s\n%s", clipped, HUB_CONTINUE_NOTE);
    hub_mem_free(clipped);
}

/*
 * Leaves the first page (at most limit characters) of a finished reply in out, followed by
 * "\nPAGE_NOTE" when more is left, and holds the rest for the next ask(). Call with the lock held.
 */
static void page(char *out, const size_t out_len, const size_t limit)
{
    if (hub_proto_utf8_chars(out) <= limit) {
        return;
    }
    const size_t note = hub_proto_utf8_chars(HUB_PAGE_NOTE) + 1;
    const size_t room = limit > note ? limit - note : 1;
    size_t cut = hub_proto_page_cut(out, room);
    const char *rest = out + cut;
    while (*rest == '\n') {
        rest++;
    }
    snprintf(s_held, sizeof(s_held), "%s", rest);
    hub_proto_utf8_trim(s_held);
    s_has_held = s_held[0] != '\0';
    s_held_at = s_ops->now_ms();
    while (cut > 0 && out[cut - 1] == '\n') {
        cut--;
    }
    out[cut] = '\0';
    if (s_has_held) {
        append(out, out_len, "\n");
        append(out, out_len, HUB_PAGE_NOTE);
    }
}

/* A safety clip: a page never exceeds limit characters. */
static void finish(char *out, const size_t out_len, const size_t limit)
{
    const size_t n = strlen(out) + 1;
    char *copy = hub_mem_alloc(n);
    if (copy) {
        memcpy(copy, out, n);
        hub_proto_clip(copy, out, out_len, limit);
        hub_mem_free(copy);
    }
}

/*
 * The held reply's next page if prompt asks for one ("続き": a page, "終わりまで": the rest up to
 * all_max_chars). Any other prompt drops the held reply. Call with the lock held.
 */
static bool take_held(const char *prompt, char *out, const size_t out_len, size_t *limit)
{
    if (!s_has_held) {
        return false;
    }
    s_has_held = false;
    const hub_key_t key = hub_proto_page_key(prompt);
    if (key == HUB_KEY_NONE || s_ops->now_ms() - s_held_at > s_hold_ms) {
        return false;
    }
    *limit = key == HUB_KEY_ALL ? s_all_max_chars : s_max_chars;
    snprintf(out, out_len, "%s", s_held);
    page(out, out_len, *limit);
    return true;
}

void hub_turns_ask(const char *prompt, char *out, const size_t out_len)
{
    s_ops->lock();
    size_t limit = s_max_chars;
    if (take_held(prompt, out, out_len, &limit)) {
        s_ops->unlock();
        finish(out, out_len, limit);
        return;
    }
    if (s_running && s_ops->now_ms() - s_started_at > s_cap_ms) {
        s_running = false;         /* the turn never ended: give up on it */
    }
    if (s_running) {
        snprintf(out, out_len, "%s", HUB_STILL_WORKING);
        s_ops->unlock();
        return;
    }
    msgs_reset();
    s_error[0] = '\0';
    s_finished = s_detached = s_sent = false;
    s_running = true;
    s_started_at = s_ops->now_ms();
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
        page(out, out_len, s_max_chars);
        s_ops->unlock();
        finish(out, out_len, s_max_chars);
        return;
    }
    s_detached = true;
    char *partial = hub_mem_alloc(HUB_REPLY_MAX);
    if (partial) {
        build(partial, HUB_REPLY_MAX);
    }
    s_ops->unlock();
    with_note(partial ? partial : "", out, out_len);
    hub_mem_free(partial);
}
