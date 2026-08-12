/**
 * @file cloud_sync_stub.c
 * @brief No-op cloud sync when CONFIG_SUPPORT_WIFI_WEB is disabled.
 */

#include "cloud_sync.h"

#include <stdio.h>
#include <string.h>

void cloud_sync_init(void) {}

esp_err_t cloud_sync_get_public_config(cloud_sync_public_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

esp_err_t cloud_sync_apply_config_json(const cJSON *root, char *err, size_t err_size)
{
    (void)root;
    if (err && err_size) {
        snprintf(err, err_size, "cloud sync unavailable (Wi-Fi/web disabled)");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cloud_sync_test(char *message, size_t message_size)
{
    if (message && message_size) {
        snprintf(message, message_size, "cloud sync unavailable");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cloud_sync_start_run(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void cloud_sync_get_status(cloud_sync_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->phase, sizeof(out->phase), "off");
        snprintf(out->last_message, sizeof(out->last_message), "unavailable");
    }
}

bool cloud_sync_is_busy(void)
{
    return false;
}

esp_err_t cloud_sync_json_config(cJSON *root)
{
    (void)root;
    return ESP_ERR_NOT_SUPPORTED;
}
