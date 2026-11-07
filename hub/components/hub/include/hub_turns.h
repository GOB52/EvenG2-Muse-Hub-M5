/* Typed turns for Even AI requests: a request waits a while, a slower reply is held for the next one. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_REPLY_MAX 2048

typedef struct {
    int64_t (*now_ms)(void);
    void (*lock)(void);
    void (*unlock)(void);
    void (*reset_done)(void);
    bool (*wait_done)(int timeout_ms);       /* true once signal_done was called */
    void (*signal_done)(void);
    bool (*start_turn)(const char *prompt);  /* hands prompt to Muse; false if it couldn't */
} hub_turns_ops_t;

extern const char HUB_STILL_WORKING[];
extern const char HUB_CONTINUE_NOTE[];
extern const char HUB_MSG_BUSY[];
extern const char HUB_MSG_NOT_SET_UP[];
extern const char HUB_MSG_INTERNAL[];

/* A turn still running after cap_ms is treated as failed by the next ask(). */
void hub_turns_init(const hub_turns_ops_t *ops, int wait_ms, int64_t hold_ms, int64_t cap_ms);
/* The text to answer prompt with (at most HUB_MAX_CHARS characters). Blocks up to wait_ms. */
void hub_turns_ask(const char *prompt, char *out, size_t out_len);
/* A typed turn's console line (muse_hatch_listener_t). */
void hub_turns_on_event(const char *type, const char *fields, const char *text);
bool hub_turns_busy(void);

#ifdef __cplusplus
}
#endif
