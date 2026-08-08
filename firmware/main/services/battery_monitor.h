/**
 * @file battery_monitor.h
 * @brief Periodic battery ADC sampling for the portal status bar.
 *
 * Spawns a low-priority task that reads the LiPo through the board voltage
 * divider, converts to percent via battery.c, and publishes into device_status
 * so GET /api/v1/status and the OLED home screen stay current.
 */

#pragma once

#include "esp_err.h"

esp_err_t battery_monitor_init(void);
