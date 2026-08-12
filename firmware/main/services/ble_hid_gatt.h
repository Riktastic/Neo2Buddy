/**
 * @file ble_hid_gatt.h
 * @brief HOGP keyboard via Espressif esp_hid (full HID + DIS + Battery).
 *
 * Handles connect/subscribe/passkey GAP events and exposes
 * ble_hid_gatt_send_report() for 8-byte keyboard reports.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "host/ble_hs.h"

void ble_hid_gatt_gap_event(struct ble_gap_event *event);

esp_err_t ble_hid_gatt_init(void);
esp_err_t ble_hid_gatt_send_report(const uint8_t *report, size_t len);
/** Drop the active GATT connection if any (does not clear bonds). */
void ble_hid_gatt_disconnect(void);
bool ble_hid_gatt_is_connected(void);
bool ble_hid_gatt_can_send(void);
