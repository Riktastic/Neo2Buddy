/**
 * @file neo_device.h
 * @brief Neo Manager dialogue: hello, switch applet, framed commands, block I/O.
 *
 * LAYERING
 * --------
 *   neo_file / neo_applet / neo_space  →  business operations
 *   neo_device (this file)             →  ASM session + 8-byte messages + blocks
 *   usb_host_neo                         →  USB bulk IN/OUT only
 *
 * DIALOGUE LIFECYCLE (emulates NeoTools session)
 * ----------------------------------------------
 *   1. neo_device_dialogue_start(applet_id)
 *        hello 0x01 → 2-byte version (must be >= 0x0220)
 *        reset "?ff\0reset"
 *        switch "?Switch" + applet_id → "Switched" (8 ASCII bytes)
 *   2. One or more neo_device_send_command() / read_extended() / write_extended()
 *   3. neo_device_dialogue_end() — reset again to leave ASM command mode
 *
 * BLOCK TRANSFER (payloads larger than 8 bytes)
 * ---------------------------------------------
 *   Read:  loop REQUEST_BLOCK_READ → RESPONSE_BLOCK_READ gives length + CRC
 *          → read that many raw bytes → verify 16-bit sum matches message
 *   Write: REQUEST_BLOCK_WRITE (length + CRC) → raw bytes → RESPONSE_BLOCK_WRITE_DONE
 *
 * Only one dialogue may run at a time — use neo_device_lock() from API entry points.
 */

#pragma once

#include "neo_message.h"
#include "esp_err.h"

#define NEO_DEFAULT_TIMEOUT_MS 1000
#define NEO_BLOCK_SIZE 0x400 /**< 1 KiB per BLOCK_READ / BLOCK_WRITE chunk (NeoTools default). */

esp_err_t neo_device_read(uint8_t *buffer, size_t length, int timeout_ms, size_t *out_length);
esp_err_t neo_device_read_exact(uint8_t *buffer, size_t length, int timeout_ms, size_t *out_length);
esp_err_t neo_device_write(const uint8_t *buffer, size_t length, int timeout_ms);

esp_err_t neo_device_send_message(const neo_message_t *msg);
esp_err_t neo_device_receive_message(neo_message_t *msg, int timeout_ms);
esp_err_t neo_device_send_command(const neo_message_t *request, uint8_t expected_response,
                                  int timeout_ms, neo_message_t *response);

/** Open ASM session and select applet (usually 0x0000 system, or target applet). */
esp_err_t neo_device_dialogue_start(uint16_t applet_id);
esp_err_t neo_device_dialogue_end(void);
esp_err_t neo_device_query_version_message(neo_message_t *response);

esp_err_t neo_device_read_extended(uint8_t *buffer, size_t capacity, size_t expected_length,
                                   size_t *out_length);
esp_err_t neo_device_write_extended(const uint8_t *buffer, size_t length);
/** Applet install: after each BLOCK_WRITE_DONE send PROGRAMMING_APPLET_BLOCK (NeoTools). */
esp_err_t neo_device_write_applet_content(const uint8_t *buffer, size_t length);

/** 16-bit truncating sum of all bytes — used in block messages and attribute payloads. */
uint16_t neo_device_data_checksum(const uint8_t *buffer, size_t length);

/** GET_FILE_ATTRIBUTES via system applet dialogue (40-byte struct). */
esp_err_t neo_device_read_file_attributes(uint16_t applet_id, uint8_t index, uint8_t *buf, size_t buf_len);

/**
 * GET_FILE_ATTRIBUTES while an ASM dialogue is already open (and locked).
 * Does not start/end a dialogue or take the protocol lock.
 */
esp_err_t neo_device_get_file_attributes_open(uint16_t applet_id, uint8_t index, uint8_t *buf, size_t buf_len);

/** REQUEST_RESTART — Neo re-enumerates as keyboard (PID 0xBD04). */
esp_err_t neo_device_restart(void);

void neo_device_lock(void);
void neo_device_unlock(void);
