#include "hub_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"

#include "hub_proto.h"
#include "hub_turns.h"

#define AUTH_HEADER_MAX 192
#define SERVER_STACK 8192
#define RECV_RETRIES 3

static const char *TAG = "hub.http";
static const char *const PATHS[] = { "/v1/chat/completions", "/chat/completions" };
static httpd_handle_t s_server;
static const char *s_token;

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":{\"message\":\"%s\"}}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static bool authorized(httpd_req_t *req)
{
    char header[AUTH_HEADER_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    return hub_proto_check_auth(header, s_token);
}

/* Clients probe the endpoint with a GET when it is saved. */
static esp_err_t probe_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"object\":\"muse-hub\",\"status\":\"ok\",\"model\":\"muse\"}");
}

static char *read_body(httpd_req_t *req)
{
    const size_t len = req->content_len;
    char *body = malloc(len + 1);
    size_t got = 0;
    int retries = 0;
    while (body && got < len) {
        const int n = httpd_req_recv(req, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && retries++ < RECV_RETRIES) {
            continue;
        }
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += (size_t)n;
    }
    if (body) {
        body[got] = '\0';
    }
    return body;
}

static esp_err_t send_reply(httpd_req_t *req, const hub_request_t *r, const char *reply)
{
    char id[32];
    snprintf(id, sizeof(id), "chatcmpl-%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    const long created = (long)time(NULL);
    char *out = r->stream ? hub_proto_completion_sse(reply, r->model, created, id)
                          : hub_proto_completion_json(reply, r->model, created, id);
    if (!out) {
        return send_error(req, "500 Internal Server Error", "no memory");
    }
    httpd_resp_set_type(req, r->stream ? "text/event-stream; charset=utf-8" : "application/json; charset=utf-8");
    const esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static esp_err_t completion_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    if (req->content_len == 0) {
        return send_error(req, "400 Bad Request", "empty body");
    }
    if (req->content_len > HUB_MAX_BODY) {
        return send_error(req, "413 Payload Too Large", "body too large");
    }
    char *body = read_body(req);
    if (!body) {
        return send_error(req, "400 Bad Request", "unreadable body");
    }
    hub_request_t *r = malloc(sizeof(*r));
    const hub_req_err_t perr = r ? hub_proto_parse_request(body, req->content_len, r) : HUB_REQ_BAD_JSON;
    free(body);
    if (perr != HUB_REQ_OK) {
        free(r);
        return send_error(req, "400 Bad Request", perr == HUB_REQ_NO_USER ? "no user message" : "bad json");
    }
    char *reply = malloc(HUB_REPLY_MAX);
    if (!reply) {
        free(r);
        return send_error(req, "500 Internal Server Error", "no memory");
    }
    ESP_LOGI(TAG, "turn: %u bytes", (unsigned)strlen(r->prompt));
    hub_turns_ask(r->prompt, reply, HUB_REPLY_MAX);
    const esp_err_t err = send_reply(req, r, reply);
    free(reply);
    free(r);
    return err;
}

esp_err_t hub_http_start(const uint16_t port, const char *token)
{
    if (strlen(token) < HUB_MIN_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    s_token = token;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.stack_size = SERVER_STACK;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");
    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        const httpd_uri_t get = { .uri = PATHS[i], .method = HTTP_GET, .handler = probe_handler };
        const httpd_uri_t post = { .uri = PATHS[i], .method = HTTP_POST, .handler = completion_handler };
        httpd_register_uri_handler(s_server, &get);
        httpd_register_uri_handler(s_server, &post);
    }
    return ESP_OK;
}
