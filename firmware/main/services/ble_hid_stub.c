/**
 * @file ble_hid_stub.c
 * @brief No-op BLE HID when CONFIG_SUPPORT_BLE is disabled.
 */

#include "ble_hid.h"

#include <string.h>

void ble_hid_hold_controller_ram(void) {}

bool ble_hid_radio_critical(void)
{
    return false;
}

void ble_hid_boot(void) {}

void ble_hid_init(void) {}

void ble_hid_deinit(void) {}

void ble_hid_start_advertising(bool allow_pairing)
{
    (void)allow_pairing;
}

void ble_hid_stop_advertising(void) {}

esp_err_t ble_hid_set_pairing_enabled(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_hid_is_ready(void)
{
    return false;
}

bool ble_hid_is_connected(void)
{
    return false;
}

bool ble_hid_is_advertising(void)
{
    return false;
}

bool ble_hid_pairing_enabled(void)
{
    return false;
}

bool ble_hid_can_send(void)
{
    return false;
}

bool ble_hid_send_in_progress(void)
{
    return false;
}

int ble_hid_bonded_count(void)
{
    return 0;
}

int ble_hid_list_bonds(ble_hid_bond_peer_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

void ble_hid_clear_bonds(void) {}

void ble_hid_passthrough_report(const uint8_t *report, size_t len)
{
    (void)report;
    (void)len;
}

esp_err_t ble_hid_preview_text(const char *text, char *preview_out, size_t preview_size, size_t *total_len)
{
    (void)text;
    if (preview_out && preview_size) {
        preview_out[0] = '\0';
    }
    if (total_len) {
        *total_len = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_hid_confirm_send(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ble_hid_cancel_send(void) {}

esp_err_t ble_hid_send_text(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}
