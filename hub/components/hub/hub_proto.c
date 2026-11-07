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
    char *text = malloc(HUB_PROMPT_MAX);
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
    free(text);
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
        bytes += seq_len((unsigned char)in[bytes]);
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

static cJSON *chunk_base(const char *object, const char *model, const long created, const char *id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "object", object);
    cJSON_AddNumberToObject(root, "created", (double)created);
    cJSON_AddStringToObject(root, "model", model);
    return root;
}

static char *print_and_free(cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return NULL;
    }
    char *out = strdup(printed);        /* plain malloc'd copy: callers free() it */
    cJSON_free(printed);
    return out;
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
        out = malloc(n);
        if (out) {
            snprintf(out, n, "data: %s\n\ndata: %s\n\ndata: [DONE]\n\n", first, last);
        }
    }
    free(first);
    free(last);
    return out;
}
