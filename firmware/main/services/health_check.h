/**
 * @file health_check.h
 * @brief USB connection watchdog (optional auto-reboot).
 *
 * Spawns a background task that tracks how long the Neo has been disconnected.
 * If USB stays absent for USB_LOST_RESTART_THRESHOLD_SECONDS (10 minutes),
 * the ESP32 reboots to recover from a wedged USB host stack. Logs state changes
 * at INFO so field logs show connect/disconnect timing.
 */

#pragma once

#include "esp_err.h"

esp_err_t health_check_init(void);
