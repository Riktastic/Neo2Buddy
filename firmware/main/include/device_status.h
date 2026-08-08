/**
 * @file device_status.h
 * @brief Mutex-protected runtime snapshot for status API and OLED.
 *
 * Individual services (battery_monitor, wifi_manager, sd_card, ble_hid) push
 * updates; web_api_http and display read device_status_get() without touching
 * hardware directly. Keeps GET /api/v1/status fast and consistent.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DEVICE_WIFI_UNCONFIGURED,
    DEVICE_WIFI_CONNECTING,
    DEVICE_WIFI_CONNECTED,
    DEVICE_WIFI_ERROR,
} device_wifi_state_t;

typedef enum {
    DEVICE_BLE_IDLE,
    DEVICE_BLE_PAIRING,
    DEVICE_BLE_CONNECTED,
} device_ble_state_t;

/** Compact snapshot of runtime device status. */
typedef struct {
    uint16_t battery_mv;
    uint8_t battery_percent;
    bool charging;
    device_wifi_state_t wifi_state;
    device_ble_state_t ble_state;
    size_t storage_total_bytes;
    size_t storage_used_bytes;
    bool sd_card_mounted;
    size_t sd_card_total_bytes;
    size_t sd_card_used_bytes;
    char wifi_ssid[33];
    char ip_address[16];
} device_status_t;

void device_status_init(void);
void device_status_get(device_status_t *status);
void device_status_set_battery(uint16_t millivolts, uint8_t percent, bool charging);
void device_status_set_wifi(device_wifi_state_t state, const char *ssid, const char *ip_address);
void device_status_set_ble(device_ble_state_t state);
void device_status_set_storage(size_t used_bytes, size_t total_bytes);
void device_status_set_sd_card(bool mounted, size_t used_bytes, size_t total_bytes);
