/**
 * @file log_buffer.h
 * @brief In-memory ring buffer of recent log lines for the web portal.
 *
 * Services and the Neo stack call log_buffer_appendf() so GET /api/v1/logs can
 * show a live tail without UART. Mutex-protected; drops lines under contention
 * rather than blocking USB or Wi-Fi tasks. JSON export powers the portal log
 * panel with level filtering.
 */

#pragma once

#include "esp_err.h"

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
} log_level_t;

esp_err_t log_buffer_init(void);

/** Append a formatted line at INFO level (convenience wrapper). */
void log_buffer_appendf(const char *fmt, ...);

/** Append a formatted line with an explicit severity. */
void log_buffer_append_level(log_level_t level, const char *fmt, ...);

/**
 * Export recent entries as JSON array:
 *   [{"ts_ms":...,"level":"INFO","msg":"..."}, ...] newest-first.
 */
esp_err_t log_buffer_get_recent_json(char *out, size_t out_len, int max_lines, log_level_t min_level);
