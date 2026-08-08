/**
 * @file factory_reset.h
 * @brief Restore firmware defaults and wipe local user data (not SD card).
 *
 * Erases NVS settings (device, auth, cloud_sync), deletes internal backup
 * files under /spiflash/neo, then reboots. SD card files are never touched.
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Verify portal password and reset the buddy to factory defaults.
 * On success this function reboots and does not return.
 */
esp_err_t factory_reset_perform(const char *password, char *err, size_t err_size);

/**
 * Wipe user data and reboot without password checks.
 * Intended for callers that already verified the password and sent a response.
 */
void factory_reset_execute(void);
