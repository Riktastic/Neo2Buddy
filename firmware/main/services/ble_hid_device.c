/**
 * @file ble_hid_device.c
 * @brief ASCII to HID keyboard reports with press/release timing.
 */

#include "ble_hid_device.h"
#include "ble_hid_gatt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "ble_hid_device";

static esp_err_t send_key_report(uint8_t modifier, uint8_t keycode)
{
    uint8_t press[8] = { modifier, 0, keycode, 0, 0, 0, 0, 0 };
    uint8_t release[8] = { 0 };

    esp_err_t err = ble_hid_gatt_send_report(press, sizeof(press));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(8));
    return ble_hid_gatt_send_report(release, sizeof(release));
}

static bool map_char_to_hid(char c, uint8_t *modifier, uint8_t *keycode)
{
    *modifier = 0;
    *keycode = 0;

    if (c >= 'a' && c <= 'z') {
        *keycode = 0x04 + (c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *modifier = 0x02;
        *keycode = 0x04 + (tolower((int)c) - 'a');
        return true;
    }
    if (c >= '1' && c <= '9') {
        *keycode = 0x1e + (c - '1');
        return true;
    }
    if (c == '0') {
        *keycode = 0x27;
        return true;
    }
    if (c == ' ') {
        *keycode = 0x2c;
        return true;
    }
    if (c == '\n' || c == '\r') {
        *keycode = 0x28;
        return true;
    }
    if (c == '\t') {
        *keycode = 0x2b;
        return true;
    }
    if (c == ',') { *keycode = 0x36; return true; }
    if (c == '.') { *keycode = 0x37; return true; }
    if (c == '-') { *keycode = 0x2d; return true; }
    if (c == '_') { *modifier = 0x02; *keycode = 0x2d; return true; }
    if (c == ':') { *modifier = 0x02; *keycode = 0x33; return true; }
    if (c == ';') { *keycode = 0x33; return true; }
    if (c == '?') { *modifier = 0x02; *keycode = 0x38; return true; }
    if (c == '/') { *keycode = 0x38; return true; }
    if (c == '!') { *modifier = 0x02; *keycode = 0x1e; return true; }
    if (c == '"') { *modifier = 0x02; *keycode = 0x34; return true; }
    if (c == '\'') { *keycode = 0x34; return true; }
    return false;
}

void ble_hid_device_send_char(char c)
{
    if (!ble_hid_gatt_can_send()) {
        return;
    }

    uint8_t modifier = 0;
    uint8_t keycode = 0;
    if (!map_char_to_hid(c, &modifier, &keycode)) {
        ESP_LOGW(TAG, "No HID mapping for 0x%02x", (uint8_t)c);
        return;
    }

    esp_err_t err = send_key_report(modifier, keycode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HID report failed: %s", esp_err_to_name(err));
    }
}

void ble_hid_device_send_string(const char *s)
{
    if (!s) {
        return;
    }
    while (*s) {
        if (!ble_hid_gatt_can_send()) {
            break;
        }
        ble_hid_device_send_char(*s++);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

void ble_hid_device_send_report(const uint8_t *report, size_t len)
{
    if (!report || len < 8) {
        return;
    }
    if (!ble_hid_gatt_can_send()) {
        return;
    }
    esp_err_t err = ble_hid_gatt_send_report(report, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "passthrough report failed: %s", esp_err_to_name(err));
    }
}
