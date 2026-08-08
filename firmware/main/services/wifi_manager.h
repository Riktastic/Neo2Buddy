/**
 * @file wifi_manager.h
 * @brief Wi-Fi: Direct access (soft-AP) and Home network (STA) modes.
 *
 * network_mode in settings selects behavior at boot. Home mode uses STA-only
 * (no concurrent AP+STA brownouts). Failed STA connect can fall back to a
 * recovery hotspot. SNTP starts after STA gets an address (needed for cloud
 * sync SigV4). Event handlers defer heavy work to a worker task.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Initialize the Wi-Fi manager. Loads saved credentials via `settings` and
 *  either starts an AP for onboarding or attempts to connect as a station.
 */
esp_err_t wifi_manager_init(void);

/** Trigger a connection attempt to the provided SSID/password. Returns
 *  ESP_OK on success (connection started), or an error code otherwise.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/** Start the onboarding AP (useful to force AP mode). */
esp_err_t wifi_manager_start_ap(void);

/** Start the device hotspot after home Wi-Fi join failures (recovery). */
esp_err_t wifi_manager_start_recovery_ap(void);

/** Synchronously start recovery AP (call only from a task context, not Wi-Fi events). */
esp_err_t wifi_manager_force_recovery_ap(void);

/** Stop any running AP and disconnect station. */
esp_err_t wifi_manager_stop(void);

/** Query whether the device is currently connected to Wi-Fi. */
bool wifi_manager_is_connected(void);

/** True when home Wi-Fi failed and the device hotspot is up for reconfiguration. */
bool wifi_manager_is_recovery_mode(void);
