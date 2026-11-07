#include "hub_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define BEARER "Bearer "
#define ELLIPSIS "\xE2\x80\xA6"   /* … */
#define DEFAULT_MODEL "muse"

static bool is_space(const char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Copies src into dst (cap bytes) without leading/trailing spaces; utf-8 safe. */
static void copy_trimmed(char *dst, const size_t cap, const char *src)
{
    while (*src && is_space(*src)) {
        src++;
    }
    size_t n = strlen(src);
    while (n && is_space(src[n - 1])) {
        n--;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    hub_proto_utf8_trim(dst);
}

static void append_text(char *dst, const size_t cap, const char *src)
{
    const size_t used = strlen(dst);
    if (used + 1 < cap) {
        snprintf(dst + used, cap - used, "%s", src);
    }
}

/* The text of a message's content: a string, or the "text" parts of an array. */
static void content_text(const cJSON *content, char *out, const size_t cap)
{
    out[0] = '\0';
    if (cJSON_IsString(content)) {
        append_text(out, cap, content->valuestring);
        return;
    }
    if (!cJSON_IsArray(content)) {
        return;
    }
    const cJSON *part = NULL;
    cJSON_ArrayForEach(part, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text)) {
            append_text(out, cap, text->valuestring);
        }
    }
}

hub_req_err_t hub_proto_parse_request(const char *body, const size_t len, hub_request_t *req)
{
    memset(req, 0, sizeof(*req));
    snprintf(req->model, sizeof(req->model), "%s", DEFAULT_MODEL);
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return HUB_REQ_BAD_JSON;
    }
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (cJSON_IsString(model) && model->valuestring[0]) {
        snprintf(req->model, sizeof(req->model), "%s", model->valuestring);
        hub_proto_utf8_trim(req->model);
    }
    req->stream = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stream"));

    hub_req_err_t err = HUB_REQ_NO_USER;
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    const int count = cJSON_IsArray(messages) ? cJSON_GetArraySize(messages) : 0;
    char *text = cJSON_malloc(HUB_PROMPT_MAX);
    for (int i = count - 1; text && i >= 0; i--) {
        const cJSON *m = cJSON_GetArrayItem(messages, i);
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
        if (!cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0) {
            continue;
        }
        content_text(cJSON_GetObjectItemCaseSensitive(m, "content"), text, HUB_PROMPT_MAX);
        copy_trimmed(req->prompt, sizeof(req->prompt), text);
        if (req->prompt[0]) {
            err = HUB_REQ_OK;
            break;
        }
    }
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

bool hub_proto_check_auth(const char *header, const char *token)
{
    const size_t prefix = sizeof(BEARER) - 1;
    const size_t token_len = strlen(token);
    if (token_len == 0 || strncmp(header, BEARER, prefix) != 0) {
        return false;
    }
    const char *cred = header + prefix;
    const size_t cred_len = strlen(cred);
    unsigned diff = (unsigned)(cred_len ^ token_len);
    for (size_t i = 0; i < token_len; i++) {
        const char c = i < cred_len ? cred[i] : 0;
        diff |= (unsigned)(unsigned char)(c ^ token[i]);
    }
    return diff == 0;
}

static bool is_continuation(const unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

size_t hub_proto_utf8_chars(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!is_continuation(*p)) {
            n++;
        }
    }
    return n;
}

/* Bytes a sequence starting with lead takes; 1 for anything invalid. */
static size_t seq_len(const unsigned char lead)
{
    if (lead >= 0xF0) {
        return 4;
    }
    if (lead >= 0xE0) {
        return 3;
    }
    if (lead >= 0xC0) {
        return 2;
    }
    return 1;
}

void hub_proto_utf8_trim(char *s)
{
    size_t n = strlen(s);
    size_t start = n;
    while (start > 0 && is_continuation((unsigned char)s[start - 1])) {
        start--;
    }
    if (start > 0 && start - 1 + seq_len((unsigned char)s[start - 1]) > n) {
        s[start - 1] = '\0';
    }
}

void hub_proto_clip(const char *in, char *out, const size_t out_len, const size_t max_chars)
{
    if (out_len == 0) {
        return;
    }
    const bool cut = hub_proto_utf8_chars(in) > max_chars;
    const size_t keep_chars = cut ? (max_chars ? max_chars - 1 : 0) : max_chars;
    size_t bytes = 0;
    size_t chars = 0;
    while (in[bytes] && chars < keep_chars) {
        bytes++;                       /* one character per lead byte, then its continuation bytes */
        while (in[bytes] && is_continuation((unsigned char)in[bytes])) {
            bytes++;
        }
        chars++;
    }
    const size_t room = out_len - 1 - (cut ? sizeof(ELLIPSIS) - 1 : 0);
    if (out_len - 1 < (cut ? sizeof(ELLIPSIS) - 1 : 0)) {
        out[0] = '\0';
        return;
    }
    if (bytes > room) {
        bytes = room;
    }
    memcpy(out, in, bytes);
    out[bytes] = '\0';
    hub_proto_utf8_trim(out);
    if (cut) {
        strcat(out, ELLIPSIS);
    }
}

#define IDEOGRAPHIC_SPACE "\xE3\x80\x80"   /* U+3000 */
#define IDEOGRAPHIC_SPACE_LEN (sizeof(IDEOGRAPHIC_SPACE) - 1)

/* Bytes of the blank character s starts with; 0 if it starts with none. */
static size_t blank_at(const char *s)
{
    if (is_space(*s)) {
        return 1;
    }
    return strncmp(s, IDEOGRAPHIC_SPACE, IDEOGRAPHIC_SPACE_LEN) == 0 ? IDEOGRAPHIC_SPACE_LEN : 0;
}

size_t hub_proto_blank_len(const char *s)
{
    size_t n = 0;
    for (size_t b = blank_at(s); b > 0; b = blank_at(s + n)) {
        n += b;
    }
    return n;
}

bool hub_proto_is_blank(const char *s)
{
    return s[hub_proto_blank_len(s)] == '\0';
}

size_t hub_proto_unblank_len(const char *s, size_t n)
{
    while (n > 0) {
        if (is_space(s[n - 1])) {
            n--;
        } else if (n >= IDEOGRAPHIC_SPACE_LEN &&
                   memcmp(s + n - IDEOGRAPHIC_SPACE_LEN, IDEOGRAPHIC_SPACE, IDEOGRAPHIC_SPACE_LEN) == 0) {
            n -= IDEOGRAPHIC_SPACE_LEN;
        } else {
            break;
        }
    }
    return n;
}

/* Words that ask for held pages, and what may follow them; edit here. */
static const char *const PAGE_ALL_WORDS[] = {
    "終わりまで", "おわりまで", "最後まで", "さいごまで", "全部", "ぜんぶ", "全文",
    "続きを全部", "続き全部", "残り全部", "残り",
};
static const char *const PAGE_CONTINUE_WORDS[] = {"続き", "つづき", "続けて", "つづけて", "次", "つぎ", "もっと"};
static const char *const PAGE_KEY_SUFFIXES[] = {
    "", "を", "を見せて", "をみせて", "を教えて", "をおしえて", "をお願い", "をおねがい", "をください",
    "お願い", "おねがい", "見せて", "みせて", "して", "のページ", "ページ", "読んで", "よんで",
    "を読んで", "をよんで",
};
/* Ignored when matching them: spaces and punctuation. */
static const char *const PAGE_KEY_IGNORED[] = {
    " ", "\t", "\n", "\r", "　", "。", "、", "．", "，", "！", "？", "!", "?", ".", ",",
    "「", "」", "『", "』", "…", "ー", "〜", "~",
};
#define PAGE_KEY_MAX 64            /* bytes of a key; more than any word with a suffix */
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static bool is_one_of(const char *c, const size_t n, const char *const *set, const size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strlen(set[i]) == n && memcmp(c, set[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* text without the ignored characters; false if that does not fit in cap bytes. */
static bool page_key_text(const char *text, char *key, const size_t cap)
{
    size_t used = 0;
    for (size_t i = 0; text[i];) {
        const size_t start = i++;
        while (text[i] && is_continuation((unsigned char)text[i])) {
            i++;
        }
        const size_t n = i - start;
        if (is_one_of(text + start, n, PAGE_KEY_IGNORED, COUNT_OF(PAGE_KEY_IGNORED))) {
            continue;
        }
        if (used + n >= cap) {
            return false;
        }
        memcpy(key + used, text + start, n);
        used += n;
    }
    key[used] = '\0';
    return true;
}

/* The suffixes are compared without the ignored characters too ("ページ" has a "ー"). */
static bool is_suffix(const char *rest)
{
    char suffix[PAGE_KEY_MAX];
    for (size_t i = 0; i < COUNT_OF(PAGE_KEY_SUFFIXES); i++) {
        if (page_key_text(PAGE_KEY_SUFFIXES[i], suffix, sizeof(suffix)) && strcmp(rest, suffix) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_word_with_suffix(const char *key, const char *const *words, const size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const size_t n = strlen(words[i]);
        if (strncmp(key, words[i], n) == 0 && is_suffix(key + n)) {
            return true;
        }
    }
    return false;
}

hub_key_t hub_proto_page_key(const char *prompt)
{
    char key[PAGE_KEY_MAX];
    if (!page_key_text(prompt, key, sizeof(key))) {
        return HUB_KEY_NONE;
    }
    if (is_word_with_suffix(key, PAGE_ALL_WORDS, COUNT_OF(PAGE_ALL_WORDS))) {
        return HUB_KEY_ALL;
    }
    if (is_word_with_suffix(key, PAGE_CONTINUE_WORDS, COUNT_OF(PAGE_CONTINUE_WORDS))) {
        return HUB_KEY_CONTINUE;
    }
    return HUB_KEY_NONE;
}

/* A page is cut right after one of these. */
static const char *const PAGE_BREAKS[] = {"。", "！", "？", "\n"};

size_t hub_proto_page_cut(const char *text, const size_t room)
{
    const size_t tail_from = room - room / 5;    /* characters before the last 20% */
    size_t bytes = 0;
    size_t chars = 0;
    size_t best = 0;
    while (text[bytes] && chars < room) {
        const size_t start = bytes++;
        while (text[bytes] && is_continuation((unsigned char)text[bytes])) {
            bytes++;
        }
        chars++;
        if (chars > tail_from && is_one_of(text + start, bytes - start, PAGE_BREAKS, COUNT_OF(PAGE_BREAKS))) {
            best = bytes;
        }
    }
    return text[bytes] && best ? best : bytes;
}

static cJSON *chunk_base(const char *object, const char *model, const long created, const char *id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "object", object);
    cJSON_AddNumberToObject(root, "created", (double)created);
    cJSON_AddStringToObject(root, "model", model);
    return root;
}

/* cJSON allocates through the SDK's hooks (PSRAM); callers free with hub_proto_free(). */
static char *print_and_free(cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return printed;
}

char *hub_proto_completion_json(const char *text, const char *model, const long created, const char *id)
{
    cJSON *root = chunk_base("chat.completion", model, created, id);
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);
    cJSON *message = cJSON_AddObjectToObject(choice, "message");
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", text);
    cJSON_AddStringToObject(choice, "finish_reason", "stop");
    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON_AddItemToArray(choices, choice);
    cJSON *usage = cJSON_AddObjectToObject(root, "usage");
    cJSON_AddNumberToObject(usage, "prompt_tokens", 0);
    cJSON_AddNumberToObject(usage, "completion_tokens", 0);
    cJSON_AddNumberToObject(usage, "total_tokens", 0);
    return print_and_free(root);
}

static char *sse_chunk(const char *text, const char *model, const long created, const char *id)
{
    cJSON *root = chunk_base("chat.completion.chunk", model, created, id);
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);
    cJSON *delta = cJSON_AddObjectToObject(choice, "delta");
    if (text) {
        cJSON_AddStringToObject(delta, "role", "assistant");
        cJSON_AddStringToObject(delta, "content", text);
        cJSON_AddNullToObject(choice, "finish_reason");
    } else {
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
    }
    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON_AddItemToArray(choices, choice);
    return print_and_free(root);
}

char *hub_proto_completion_sse(const char *text, const char *model, const long created, const char *id)
{
    char *first = sse_chunk(text, model, created, id);
    char *last = sse_chunk(NULL, model, created, id);
    char *out = NULL;
    if (first && last) {
        const size_t n = strlen(first) + strlen(last) + 64;
        out = cJSON_malloc(n);
        if (out) {
            snprintf(out, n, "data: %s\n\ndata: %s\n\ndata: [DONE]\n\n", first, last);
        }
    }
    cJSON_free(first);
    cJSON_free(last);
    return out;
}

void hub_proto_free(void *p)
{
    cJSON_free(p);
}
