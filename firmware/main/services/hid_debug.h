/**
 * @file hid_debug.h
 * @brief Ring buffer of raw Neo keyboard HID reports (field diagnostics).
 *
 * While the Neo is in USB keyboard mode (PID 0xBD04), interrupt IN reports are
 * copied here. GET /api/v1/keyboard/raw returns recent entries as JSON hex for
 * debugging layout mapping without spamming serial on every keypress.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

/** Store one raw HID report (truncated to HID_DEBUG_MAX_REPORT_LEN). */
void hid_debug_append(const uint8_t *data, size_t len);

/** Write a JSON array of recent reports into out (newest first, up to limit). */
esp_err_t hid_debug_get_json(char *out, size_t out_size, int limit);
