/**
 * @file ble_hid.h
 * @brief BLE HID keyboard — Neo key passthrough + portal text relay.
 *
 * The buddy advertises as a standard BLE keyboard. While a host is connected
 * and subscribed, Neo USB keyboard reports are forwarded live over BLE.
 * Portal/API text can still be queued via ble_hid_preview_text() and typed with
 * ble_hid_confirm_send().
 *
 * The NimBLE stack does **not** start at every boot. Advertising begins only when:
 * - at least one host is already bonded (reconnect after reboot), or
 * - the user opens the pairing window from the portal/UART.
 *
 * Under the hood: ble_hid_gatt.c (NimBLE GATT) + ble_hid_device.c (scancode map).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Contiguous INTERNAL DRAM pinned for the BT controller before nimble_port_init.
 * NimBLE host pools use PSRAM; the controller EMI still requires INTERNAL.
 * Prefer 28 KB — enough above the controller floor, but leaves room for an
 * INTERNAL httpd stack (SPIFFS handlers cannot safely run on a PSRAM stack).
 */
#define BLE_HID_MIN_INTERNAL_HEAP 28672
/** Absolute floor — below this, nimble_port_init is unlikely to succeed. */
#define BLE_HID_CONTROLLER_FLOOR 24576
/** Max bonded hosts we expose in list APIs (matches NimBLE MAX_BONDS). */
#define BLE_HID_MAX_BONDS 3
/** Legacy fixed passkey (unused with Just Works / no-IO pairing). */
#define BLE_HID_PAIRING_PASSKEY 123456u

/**
 * Pin a contiguous INTERNAL block before Wi‑Fi/httpd/USB fragment the heap.
 * Freed automatically immediately before nimble_port_init(). Safe to call
 * repeatedly (no-op if already held or BLE already running).
 */
void ble_hid_hold_controller_ram(void);

/**
 * True while pairing is open, BLE is connecting/connected, or the stack is
 * mid-start. Callers should defer Wi‑Fi active scans and other radio-heavy
 * work so coexistence does not interrupt HID.
 */
bool ble_hid_radio_critical(void);

/** Bring up NimBLE only if bonded hosts exist; otherwise leave BLE idle. */
void ble_hid_boot(void);

/** Start the NimBLE stack (idempotent). Prefer ble_hid_boot() at power-on. */
void ble_hid_init(void);
void ble_hid_deinit(void);

void ble_hid_start_advertising(bool allow_pairing);
void ble_hid_stop_advertising(void);
/**
 * Enable pairing window (lazy-starts BLE if needed) or return to bonded-only adv.
 * Returns ESP_ERR_NO_MEM if the controller cannot start (low internal heap).
 */
esp_err_t ble_hid_set_pairing_enabled(bool enabled);

bool ble_hid_is_ready(void);
bool ble_hid_is_connected(void);
bool ble_hid_is_advertising(void);
bool ble_hid_pairing_enabled(void);
bool ble_hid_can_send(void);
bool ble_hid_send_in_progress(void);

/** Number of bonded BLE hosts stored in NVS (0 if store unavailable). */
int ble_hid_bonded_count(void);

/** One bonded peer identity address (Bluetooth device address). */
typedef struct {
    uint8_t addr[6]; /**< MSB first for display (addr[0] is most significant). */
    uint8_t type;    /**< BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM / etc. */
} ble_hid_bond_peer_t;

/**
 * Fill out[] with up to max bonded peers.
 * Uses the live NimBLE store when the stack is up; otherwise the last
 * persisted snapshot (so listing works before pairing is started).
 * Returns the number of peers written (0..max).
 */
int ble_hid_list_bonds(ble_hid_bond_peer_t *out, int max);

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
