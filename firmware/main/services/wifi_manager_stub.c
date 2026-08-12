/**
 * @file wifi_manager_stub.c
 * @brief No-op Wi-Fi manager when CONFIG_SUPPORT_WIFI_WEB is disabled.
 */

#include "wifi_manager.h"

#include <string.h>

esp_err_t wifi_manager_init(void)
{
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_connect_best(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_start_ap(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_start_recovery_ap(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_force_recovery_ap(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_stop(void)
{
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return false;
}

bool wifi_manager_is_recovery_mode(void)
{
    return false;
}

bool wifi_manager_get_ip(char *out, size_t out_len)
{
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    return false;
}
