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
#include <stdbool.h>

#include "esp_err.h"

void neo_live_init(void);
void neo_live_append(const char *text, size_t length);
void neo_live_clear(void);
/** UART keystroke mirror — off by default (expensive on SoftAP/typing). */
void neo_live_set_key_log(bool enabled);
bool neo_live_get_key_log(void);
/** Current snapshot sequence without copying text (for cheap 304 checks). */
esp_err_t neo_live_get_sequence(unsigned long *sequence);
esp_err_t neo_live_snapshot(char *buffer, size_t buffer_size, unsigned long *sequence);

/**
 * Copy the last characters into @p buffer for a glanceable display.
 * Newlines/tabs become spaces. Always NUL-terminates when buffer_size > 0.
 */
esp_err_t neo_live_tail(char *buffer, size_t buffer_size, unsigned long *sequence);
