/* The Muse side chat the gadget's turns go to: CONFIG_HUB_SIDE_CHAT_ID, else one kept in NVS. */
#include "hub_side_chat.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "hub_proto.h"
#include "muse_chat.h"

#if CONFIG_HUB_SIDE_CHAT

#define NVS_NAMESPACE "hub"
#define NVS_KEY "side_chat"
#define NVS_WAIT_MS 100
/* Internal RAM: NVS writes run with the cache off, when a PSRAM stack can't be used. */
#define SIDE_TASK_STACK 3072
#define SIDE_TASK_PRIO 4

static const char *TAG = "hub";

/* The ID kept in NVS, or a new one kept there. False if NVS fails. */
static bool kept_id(char *out, const char **source)
{
    nvs_handle_t h;
    esp_err_t err;
    /* The SDK's app_run() initializes NVS right after hub_app_start(). */
    while ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h)) == ESP_ERR_NVS_NOT_INITIALIZED) {
        vTaskDelay(pdMS_TO_TICKS(NVS_WAIT_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "side chat: nvs_open: %s", esp_err_to_name(err));
        return false;
    }
    size_t len = HUB_UUID_LEN + 1;
    if (nvs_get_str(h, NVS_KEY, out, &len) == ESP_OK && hub_proto_uuid_valid(out)) {
        nvs_close(h);
        *source = "NVS";
        return true;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    hub_proto_uuid_format(rnd, out);
    err = nvs_set_str(h, NVS_KEY, out);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "side chat: keeping the new ID: %s", esp_err_to_name(err));
        return false;
    }
    *source = "new";
    return true;
}

static void use(const char *id, const char *source)
{
    ESP_LOGI(TAG, "side chat: %s (%s)", id, source);
    muse_hatch_set_session_id(id);
}

static void side_task(void *arg)
{
    (void)arg;
    char id[HUB_UUID_LEN + 1];
    const char *source = "";
    if (kept_id(id, &source)) {
        use(id, source);
    } else {
        ESP_LOGE(TAG, "side chat: no ID; using the main chat");
    }
    vTaskDelete(NULL);
}

void hub_side_chat_start(void)
{
    const char *const configured = CONFIG_HUB_SIDE_CHAT_ID;
    if (configured[0]) {
        if (hub_proto_uuid_valid(configured)) {
            use(configured, "Kconfig");
        } else {
            ESP_LOGE(TAG, "side chat: CONFIG_HUB_SIDE_CHAT_ID is not a lowercase UUID; using the main chat");
        }
        return;
    }
    if (xTaskCreate(side_task, "hub_side", SIDE_TASK_STACK, NULL, SIDE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "side chat: no memory for the task; using the main chat");
    }
}

#else

void hub_side_chat_start(void)
{
}

#endif
