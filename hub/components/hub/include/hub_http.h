#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serves the Even AI endpoint on port; ESP_ERR_INVALID_ARG if token is shorter than HUB_MIN_TOKEN_LEN. */
esp_err_t hub_http_start(uint16_t port, const char *token);

#ifdef __cplusplus
}
#endif
