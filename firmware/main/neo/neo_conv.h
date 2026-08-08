/**
 * @file neo_conv.h
 * @brief AlphaWord charset ↔ UTF-8 (NeoTools character_map tables).
 *
 * ON-DEVICE TEXT IS NOT UTF-8
 * ---------------------------
 * AlphaWord stores a custom byte encoding (see character_map tables in this
 * folder: en-us.txt, ua-pc.txt, ua-mac.txt). Backups must convert Neo bytes → UTF-8 for .txt files on SD;
 * imports convert UTF-8 → Neo bytes before WRITE_RAW_FILE.
 *
 * Maps: en-us (default), ua-pc, ua-mac — selected by portal ?map= or CLI.
 * Export size is usually ~1:1 with raw Neo length for en-us.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NEO_CHARMAP_EN_US = 0,
    NEO_CHARMAP_UA_PC,
    NEO_CHARMAP_UA_MAC,
} neo_charmap_id_t;

neo_charmap_id_t neo_charmap_by_name(const char *name);
esp_err_t neo_charmap_init(void);

size_t neo_conv_export_buf_size(size_t neo_len);
size_t neo_conv_export_text_from_neo(const uint8_t *neo, size_t neo_len, char *out, size_t out_len,
                                     neo_charmap_id_t map);
esp_err_t neo_conv_import_text_to_neo(const char *text, neo_charmap_id_t map, uint8_t *out, size_t out_capacity,
                                    size_t *out_length);
