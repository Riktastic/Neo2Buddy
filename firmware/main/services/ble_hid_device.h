/**
 * @file ble_hid_device.h
 * @brief HID keyboard report helpers (ASCII map + raw boot reports).
 *
 * Maps printable characters to press/release report pairs for portal text send.
 * Also forwards raw 8-byte boot keyboard reports for Neo→BLE passthrough.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

void ble_hid_device_send_string(const char *s);
void ble_hid_device_send_char(char c);

/** Forward a standard 8-byte boot keyboard report (modifier + 6 keys). */
void ble_hid_device_send_report(const uint8_t *report, size_t len);
