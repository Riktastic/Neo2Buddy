/**
 * @file neo_file.h
 * @brief Per-document file operations on a SmartApplet (NeoTools parity).
 *
 * FILE MODEL ON THE NEO
 * ---------------------
 * Each applet (e.g. AlphaWord 0xA000) holds up to 8 document slots (index 1..8).
 * Slot metadata is a fixed 40-byte record returned by GET_FILE_ATTRIBUTES:
 *
 *   offset 0x00  name[15] + nul padding
 *   offset 0x10  password[7]
 *   offset 0x18  min_size (BE u32) — logical text length
 *   offset 0x1c  alloc_size (BE u32) — allocated buffer on device
 *   offset 0x20  flags (BE u32)
 *   offset 0x25  file_space code (maps to Neo "File 1".."File 8" keys)
 *
 * READ PATH (backup export)
 * -------------------------
 *   One ASM dialogue: GET_FILE_ATTRIBUTES → READ_RAW_FILE → neo_device_read_extended(alloc_size)
 *   Prefer neo_file_read_alloc() so callers avoid a second dialogue just to learn size.
 *
 * WRITE PATH (import)
 * -------------------
 *   SET_FILE_ATTRIBUTES → write_extended(40 bytes) → COMMIT
 *   → WRITE_RAW_FILE → write_extended(data) → CONFIRM_WRITE_FILE
 *
 * Empty slot: GET_FILE_ATTRIBUTES returns NEO_ERROR_PARAMETER (0x90).
 * We scan indices 1,2,3… until NOT_FOUND to build a file list.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define NEO_FILE_ATTR_SIZE 40

typedef struct {
    uint8_t file_index;
    char name[16];
    char password[8];
    uint32_t min_size;
    uint32_t alloc_size;
    uint32_t flags;
    uint8_t file_space;
    int space_number; /**< 0=unbound, 1..8 = Neo file key mapping */
} neo_file_attr_t;

int neo_file_space_to_number(uint8_t code);
uint8_t neo_file_space_from_number(int space_number);

esp_err_t neo_get_file_attributes(uint16_t applet_id, uint8_t index, neo_file_attr_t *out);
esp_err_t neo_file_list_applet(uint16_t applet_id, cJSON *files);

esp_err_t neo_file_read_raw(uint16_t applet_id, uint8_t file_index, uint8_t *buffer, size_t capacity,
                            size_t *out_length);
esp_err_t neo_file_read_raw_with_size(uint16_t applet_id, uint8_t file_index, uint32_t alloc_size,
                                      uint8_t *buffer, size_t capacity, size_t *out_length);

/**
 * Fetch attributes and file payload in a single ASM dialogue.
 * On success with non-empty content, *out_data is malloc'd (caller frees).
 * Empty files set *out_data=NULL and *out_length=0.
 * @param attrs_out optional; filled even when the file is empty.
 * @param max_bytes hard cap on allocated size (e.g. 256 KiB).
 */
esp_err_t neo_file_read_alloc(uint16_t applet_id, uint8_t file_index, neo_file_attr_t *attrs_out,
                              uint8_t **out_data, size_t *out_length, size_t max_bytes);

esp_err_t neo_file_write_raw(uint16_t applet_id, uint8_t file_index, const uint8_t *data, size_t length);
esp_err_t neo_file_clear(uint16_t applet_id, uint8_t file_index);
esp_err_t neo_file_create(uint16_t applet_id, const char *name, const char *password, const uint8_t *data,
                          size_t length);

esp_err_t neo_file_find_by_name_or_space(uint16_t applet_id, const char *name_or_space, neo_file_attr_t *out);
esp_err_t neo_file_clear_by_name_or_space(uint16_t applet_id, const char *name_or_space);
esp_err_t neo_file_write_or_create(uint16_t applet_id, const char *name_or_space, const char *password,
                                   const uint8_t *data, size_t length);
