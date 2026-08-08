/**
 * @file health_check.c
 * @brief USB disconnect watchdog — reboot after prolonged Neo absence.
 *
 * Why this exists: on long-running installs the ESP32 USB host can end up in a
 * state where the Neo is unplugged but internal flags never recover. A periodic
 * reboot after 10 minutes of "no Neo" is cheaper than requiring a manual power
 * cycle. Shorter disconnects (normal unplug) do not trigger restart.
 */

#include "health_check.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_host_neo.h"

static const char *TAG = "health_check";

#define HEALTH_CHECK_INTERVAL_SECONDS 60
/** Reboot only after this many continuous seconds without USB Neo. */
#define USB_LOST_RESTART_THRESHOLD_SECONDS 600

static void health_check_task(void *arg)
{
    (void)arg;
    bool last_seen = usb_host_neo_is_connected();
    uint64_t last_change_ts = (uint64_t)esp_timer_get_time() / 1000000ULL;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL_SECONDS * 1000));

        bool now_connected = usb_host_neo_is_connected();
        uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;

        if (now_connected != last_seen) {
            last_seen = now_connected;
            last_change_ts = now;
            ESP_LOGI(TAG, "usb connection state changed: %s", now_connected ? "connected" : "disconnected");
        }

        if (!now_connected) {
            uint64_t lost = now - last_change_ts;
            if (lost > USB_LOST_RESTART_THRESHOLD_SECONDS) {
                ESP_LOGW(TAG, "usb disconnected for %llu seconds; restarting...", (unsigned long long)lost);
                esp_restart();
            }
        }
    }
}

esp_err_t health_check_init(void)
{
    xTaskCreate(health_check_task, "health_chk", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}
