/**
 * @file neo_debug.h
 * @brief In-memory trace of Neo USB + protocol (field diagnostics).
 *
 * Every neo_debug_event() is stored in a ring buffer (always).
 * Serial / portal log mirroring is OFF unless verbose — see neo_debug_set_verbose.
 *
 * Use `neo debug` / GET /api/v1/neo/debug after a failed backup to see the exact
 * command sequence (hello, switch, GET_FILE_ATTRIBUTES, …) without spamming boot.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Initialize the in-memory Neo debug ring buffer (call once at boot). */
void neo_debug_init(void);

/** When true, protocol/event traces also go to ESP_LOG and log_buffer. */
void neo_debug_set_verbose(bool enabled);
bool neo_debug_is_verbose(void);

/** Log a text-only Neo/USB event (ring always; serial only when verbose). */
void neo_debug_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a USB bulk/control transfer with optional payload hex (truncated to 8 bytes in buffer).
 * @param direction  e.g. "usb_in", "usb_out", "usb_ctrl"
 * @param err        esp_err_to_name result or NULL
 */
void neo_debug_xfer(const char *direction, esp_err_t err, const uint8_t *data, size_t len);

/** Log an 8-byte Neo protocol message. @param direction "REQUEST", "RESPONSE", or legacy "msg_out"/"msg_in" */
void neo_debug_message(const char *direction, const uint8_t data[8]);

/** Log a non-8-byte legacy handshake or payload prefix. */
void neo_debug_raw(const char *label, const char *direction, const uint8_t *data, size_t len);

/** Log a completed request/response exchange summary. */
void neo_debug_command_exchange(const uint8_t request[8], const uint8_t response[8], uint8_t expected_response,
                                esp_err_t result);

/** Return recent entries as JSON array (newest first). */
esp_err_t neo_debug_get_json(char *out, size_t out_size, int limit);
