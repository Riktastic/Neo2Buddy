/**
 * @file device_status.c
 * @brief Thread-safe device status container used by services and the web UI.
 *
 * This module holds a compact snapshot of state (battery, Wi-Fi, BLE,
 * storage) that is updated by various subsystems and read by the web API.
 * A mutex protects concurrent access and simple setters are provided for
 * each category of state.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "device_status.h"

static device_status_t current_status;
static SemaphoreHandle_t status_lock;

/* Helper to copy optional text fields into the fixed-size buffers. */
static void copy_text(char *destination, size_t destination_size, const char *source)
{
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    strlcpy(destination, source, destination_size);
}

/* Initialize status structure and create the protecting mutex. */
void device_status_init(void)
{
    memset(&current_status, 0, sizeof(current_status));
    current_status.wifi_state = DEVICE_WIFI_UNCONFIGURED;
    current_status.ble_state = DEVICE_BLE_IDLE;
    status_lock = xSemaphoreCreateMutex();
}

/* Copy the current status into the caller-provided buffer. */
void device_status_get(device_status_t *status)
{
    if (status == NULL || status_lock == NULL) {
        return;
    }

    xSemaphoreTake(status_lock, portMAX_DELAY);
    *status = current_status;
    xSemaphoreGive(status_lock);
}

/* Set battery state. */
void device_status_set_battery(uint16_t millivolts, uint8_t percent, bool charging)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    current_status.battery_mv = millivolts;
    current_status.battery_percent = percent;
    current_status.charging = charging;
    xSemaphoreGive(status_lock);
}

/* Set Wi-Fi state and copy SSID/IP strings. */
void device_status_set_wifi(device_wifi_state_t state, const char *ssid, const char *ip_address)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    current_status.wifi_state = state;
    copy_text(current_status.wifi_ssid, sizeof(current_status.wifi_ssid), ssid);
    copy_text(current_status.ip_address, sizeof(current_status.ip_address), ip_address);
    xSemaphoreGive(status_lock);
}

/* Set BLE operational state. */
void device_status_set_ble(device_ble_state_t state)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    current_status.ble_state = state;
    xSemaphoreGive(status_lock);
}

/* Update storage usage snapshot. */
void device_status_set_storage(size_t used_bytes, size_t total_bytes)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    current_status.storage_used_bytes = used_bytes;
    current_status.storage_total_bytes = total_bytes;
    xSemaphoreGive(status_lock);
}

/* Update SD card mount and usage fields. */
void device_status_set_sd_card(bool mounted, size_t used_bytes, size_t total_bytes)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    current_status.sd_card_mounted = mounted;
    current_status.sd_card_used_bytes = used_bytes;
    current_status.sd_card_total_bytes = total_bytes;
    xSemaphoreGive(status_lock);
}
