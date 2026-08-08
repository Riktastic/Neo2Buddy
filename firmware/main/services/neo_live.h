/**
 * @file neo_live.h
 * @brief Rolling buffer of text typed on the Neo in USB keyboard mode.
 *
 * HID reports are decoded to ASCII and appended here. GET /api/v1/keyboard/recent
 * returns a snapshot for the portal “live typing” panel. Thread-safe; cleared
 * when the user hits “clear” in the UI or via keyboard/clear API.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

void neo_live_init(void);
void neo_live_append(const char *text, size_t length);
void neo_live_clear(void);
esp_err_t neo_live_snapshot(char *buffer, size_t buffer_size, unsigned long *sequence);

/**
 * Copy the last characters into @p buffer for a glanceable display.
 * Newlines/tabs become spaces. Always NUL-terminates when buffer_size > 0.
 */
esp_err_t neo_live_tail(char *buffer, size_t buffer_size, unsigned long *sequence);
