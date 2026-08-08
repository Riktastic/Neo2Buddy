/**
 * @file ble_hid_gatt.h
 * @brief NimBLE GATT HID keyboard service (report characteristic).
 *
 * Registers the standard HID-over-GATT profile, handles connect/disconnect GAP
 * events, and exposes ble_hid_gatt_send_report() for 8-byte keyboard reports.
 * Connection state gates whether the portal may start a send job.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "host/ble_hs.h"

void ble_hid_gatt_gap_event(struct ble_gap_event *event);

esp_err_t ble_hid_gatt_init(void);
esp_err_t ble_hid_gatt_send_report(const uint8_t *report, size_t len);
bool ble_hid_gatt_is_connected(void);
bool ble_hid_gatt_can_send(void);
