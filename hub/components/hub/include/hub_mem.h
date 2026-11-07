/* Per-request buffers: PSRAM on the device (internal RAM is the SDK's), the C heap on the host. */
#pragma once

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"

/* heap_caps_*: plain malloc puts blocks under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL in internal RAM. */
#define HUB_MEM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static inline void *hub_mem_alloc(const size_t n) { return heap_caps_malloc(n, HUB_MEM_CAPS); }
static inline void *hub_mem_realloc(void *p, const size_t n) { return heap_caps_realloc(p, n, HUB_MEM_CAPS); }
static inline void hub_mem_free(void *p) { heap_caps_free(p); }
#else
#include <stdlib.h>

static inline void *hub_mem_alloc(const size_t n) { return malloc(n); }
static inline void *hub_mem_realloc(void *p, const size_t n) { return realloc(p, n); }
static inline void hub_mem_free(void *p) { free(p); }
#endif
