/* Request parsing and response formatting for the Even AI endpoint. No ESP-IDF here: host-tested. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_MAX_BODY (64 * 1024)
#define HUB_PROMPT_MAX 4096
#define HUB_MODEL_MAX 64
#define HUB_MIN_TOKEN_LEN 32

typedef struct {
    char prompt[HUB_PROMPT_MAX];
    char model[HUB_MODEL_MAX];
    bool stream;
} hub_request_t;

typedef enum {
    HUB_REQ_OK = 0,
    HUB_REQ_BAD_JSON,
    HUB_REQ_NO_USER,
} hub_req_err_t;

/* The last user message's text (trimmed), the model and stream flag. */
hub_req_err_t hub_proto_parse_request(const char *body, size_t len, hub_request_t *req);

/* "Bearer <token>", compared in constant time. False for an empty token. */
bool hub_proto_check_auth(const char *header, const char *token);

size_t hub_proto_utf8_chars(const char *s);
/* Drops a trailing incomplete UTF-8 sequence in place. */
void hub_proto_utf8_trim(char *s);
/* Copies at most max_chars characters of in to out, ending with "…" when cut. */
void hub_proto_clip(const char *in, char *out, size_t out_len, size_t max_chars);

/*
 * Bytes of text's first page of at most room characters: the whole text if it fits, else cut right
 * after the last "。", "！", "？" or "\n" in the last 20% of the room, else at room characters.
 */
size_t hub_proto_page_cut(const char *text, size_t room);

/* chat.completion JSON / SSE stream for text. cJSON_malloc'd; free with hub_proto_free(). NULL on no memory. */
char *hub_proto_completion_json(const char *text, const char *model, long created, const char *id);
char *hub_proto_completion_sse(const char *text, const char *model, long created, const char *id);
void hub_proto_free(void *p);

#ifdef __cplusplus
}
#endif
