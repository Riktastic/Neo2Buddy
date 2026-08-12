/**
 * @file neo_applet.h
 * @brief SmartApplet packages (.os3kapp) and on-device applet management.
 *
 * WHAT IS AN APPLET?
 * ------------------
 * AlphaWord and other programs on the Neo are "SmartApplets" — binary blobs with:
 *   - 0x84-byte header (signature 0xC0FFEEAD, rom/ram sizes, id, name, …)
 *   - ROM image body
 *   - footer signature 0xCAFEFEED
 *
 * On-device we talk to applet 0x0000 (system) to list/install/remove others.
 * AlphaWord documents use applet id 0xA000 — always uint16_t on the wire.
 *
 * PROTOCOL COMMANDS USED (via neo_device + system dialogue)
 * ---------------------------------------------------------
 *   LIST_APPLETS   — batched headers (7 per request)
 *   READ_APPLET    — READ_FILE response + read_extended (NeoTools applets fetch)
 *   WRITE_APPLET   — reserve space, write_applet_content, FINALIZE
 *   REMOVE_APPLET / ERASE_APPLETS
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#define NEO_APPLET_HEADER_SIZE 0x84
#define NEO_APPLET_LIST_BATCH_SIZE 7
#define NEO_APPLET_ID_SYSTEM 0x0000
#define NEO_APPLET_ID_ALPHAWORD 0xA000

typedef struct {
    uint32_t rom_size;
    uint32_t ram_size;
    uint32_t settings_offset;
    uint32_t flags;
    uint16_t applet_id;
    uint8_t file_count;
    char name[37];
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t version_revision;
    uint32_t file_space;
} neo_applet_info_t;

/** Parse .os3kapp header/footer without USB (portal inspect). */
esp_err_t neo_applet_inspect(const uint8_t *content, size_t content_length, neo_applet_info_t *info);
/** LIST_APPLETS over USB into caller buffer. */
esp_err_t neo_applet_list(neo_applet_info_t *applets, size_t capacity, size_t *out_count);
esp_err_t neo_applet_remove(uint16_t applet_id);
esp_err_t neo_applet_remove_all(void);
esp_err_t neo_applet_fetch(uint16_t applet_id, uint8_t *content, size_t capacity, size_t *out_length);
esp_err_t neo_applet_install(const uint8_t *content, size_t content_length, bool replace_existing);
