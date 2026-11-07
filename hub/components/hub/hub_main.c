/* hub_app_start(): the SDK's main calls it (hub/patches/0001) before app_run(). */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "hub_http.h"
#include "hub_turns.h"
#include "muse_chat.h"
#include "muse_state.h"

#define NET_POLL_MS 1000
#define NET_TASK_STACK 4096
#define NET_TASK_PRIO 4
#define HOLD_MS (10 * 60 * 1000LL)
#define TURN_CAP_MS (16 * 60 * 1000LL)

/* The SDK's control link to Muse is up: the gadget is paired and online (main/noise_control.h). */
extern bool noise_ctrl_is_connected(void);

static const char *TAG = "hub";
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_done;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }
static void reset_done(void) { xSemaphoreTake(s_done, 0); }
static bool wait_done(const int timeout_ms) { return xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
static void signal_done(void) { xSemaphoreGive(s_done); }

static bool start_turn(const char *prompt)
{
    char *copy = strdup(prompt);       /* muse_hatch_text_turn takes and frees it */
    if (!copy) {
        return false;
    }
    muse_hatch_text_turn(copy);
    return true;
}

static const hub_turns_ops_t OPS = {
    .now_ms = now_ms, .lock = lock, .unlock = unlock, .reset_done = reset_done,
    .wait_done = wait_done, .signal_done = signal_done, .start_turn = start_turn,
};

static void start_mdns(const uint16_t port)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: no %s.local", CONFIG_HUB_MDNS_NAME);
        return;
    }
    mdns_hostname_set(CONFIG_HUB_MDNS_NAME);
    mdns_instance_name_set("Muse hub");
    mdns_service_add(NULL, "_http", "_tcp", port, NULL, 0);
}

static void announce(const esp_ip4_addr_t *ip, const uint16_t port)
{
    char addr[16];
    esp_ip4addr_ntoa(ip, addr, sizeof(addr));
    ESP_LOGI(TAG, "endpoint: http://%s:%u/v1/chat/completions (http://%s.local:%u)", addr, port,
             CONFIG_HUB_MDNS_NAME, port);
    char hint[48];
    snprintf(hint, sizeof(hint), "HUB %s:%u", addr, port);
    muse_state_set_hint(hint);
}

static uint32_t sta_ip(esp_netif_ip_info_t *info)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, info) != ESP_OK) {
        return 0;
    }
    return info->ip.addr;
}

/*
 * Starts the endpoint once the gadget is paired and online: during setup the
 * SDK needs the internal RAM (its Wi-Fi scan task) that the server would hold.
 */
static void net_task(void *arg)
{
    const uint16_t port = CONFIG_HUB_HTTP_PORT;
    uint32_t last = 0;
    bool started = false;
    for (;;) {
        esp_netif_ip_info_t info;
        const uint32_t addr = sta_ip(&info);
        if (!started && addr && noise_ctrl_is_connected()) {
            const esp_err_t err = hub_http_start(port, CONFIG_HUB_RELAY_TOKEN);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "HTTP server not started (%s): set CONFIG_HUB_RELAY_TOKEN (32+ chars)",
                         esp_err_to_name(err));
                for (;;) {
                    vTaskDelay(portMAX_DELAY);   /* a task with a PSRAM stack can't delete itself */
                }
            }
            start_mdns(port);
            started = true;
        }
        if (started && addr && addr != last) {
            last = addr;
            announce(&info.ip, port);
        }
        vTaskDelay(pdMS_TO_TICKS(NET_POLL_MS));
    }
}

void hub_app_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_mutex || !s_done) {
        ESP_LOGE(TAG, "no memory for the hub");
        return;
    }
    hub_turns_init(&OPS, CONFIG_HUB_WAIT_S * 1000, HOLD_MS, TURN_CAP_MS, CONFIG_HUB_MAX_CHARS);
    muse_hatch_set_listener(hub_turns_on_event);
    /* Stacks in PSRAM: the SDK's own tasks need the internal RAM. */
    if (xTaskCreateWithCaps(net_task, "hub_net", NET_TASK_STACK, NULL, NET_TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the hub task");
    }
}
