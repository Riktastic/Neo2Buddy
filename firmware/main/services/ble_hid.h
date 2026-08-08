/**
 * @file ble_hid.h
 * @brief BLE HID keyboard — Neo key passthrough + portal text relay.
 *
 * The buddy advertises as a standard BLE keyboard. While a host is connected
 * and subscribed, Neo USB keyboard reports are forwarded live over BLE.
 * Portal/API text can still be queued via ble_hid_preview_text() and typed with
 * ble_hid_confirm_send(). Bonds persist in NVS across reboot; after boot the
 * buddy re-advertises so a previously paired host can reconnect without a new
 * pairing window. The pairing window is only required to bond a *new* host.
 *
 * Under the hood: ble_hid_gatt.c (NimBLE GATT) + ble_hid_device.c (scancode map).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void ble_hid_init(void);
void ble_hid_deinit(void);

void ble_hid_start_advertising(bool allow_pairing);
void ble_hid_stop_advertising(void);
void ble_hid_set_pairing_enabled(bool enabled);

bool ble_hid_is_connected(void);
bool ble_hid_is_advertising(void);
bool ble_hid_pairing_enabled(void);
bool ble_hid_can_send(void);
bool ble_hid_send_in_progress(void);

/** Number of bonded BLE hosts stored in NVS (0 if store unavailable). */
int ble_hid_bonded_count(void);

/** Erase all stored BLE bonds (used by factory reset). */
void ble_hid_clear_bonds(void);

/**
 * Queue a Neo USB boot-keyboard report for BLE passthrough.
 * Safe to call from the USB host callback; drops reports if the queue is full
 * or a portal send job is running.
 */
void ble_hid_passthrough_report(const uint8_t *report, size_t len);

/** Queue text for preview; does not transmit until confirmed. */
esp_err_t ble_hid_preview_text(const char *text, char *preview_out, size_t preview_size, size_t *total_len);
esp_err_t ble_hid_confirm_send(void);
void ble_hid_cancel_send(void);

/** Immediate send (used after confirmation). Returns ESP_ERR_INVALID_STATE if not connected. */
esp_err_t ble_hid_send_text(const char *text);
