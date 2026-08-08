/**
 * @file neo_import.h
 * @brief Helpers to save NEO documents to the local filesystem.
 *
 * Filenames look like:
 *   {deviceId}_s{NN}_{fileName}_{YYYYMMDD}.txt
 *
 * Why this scheme:
 *   - Neo exposes no usable MAC/serial for labeling backups.
 *   - deviceId = settings neo_label, else Buddy device_name (classroom label).
 *   - sNN keeps slots distinct when Neo names are empty or duplicated.
 *   - YYYYMMDD (or "nodate" before SNTP) versions by day without a DB.
 *
 * Content policy (neo_import.c / neo_import_text.c):
 *   - Skip blank / whitespace-only documents.
 *   - Skip when any existing backup already has identical UTF-8 bytes.
 *   - Prune oldest `.txt` files when free space or count limits are hit.
 *
 * Unchanged detection can also compare today's path via neo_import_file_matches
 * (full-byte compare — simpler than a separate hash store).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "neo_conv.h"

#define NEO_IMPORT_DIRECTORY "/sdcard/neo"
#define NEO_DOCUMENT_NAME_MAX_LENGTH 48
#define NEO_IMPORT_DEVICE_ID_MAX 32

typedef struct {
    uint8_t file_index;
    const char *file_name;
    const char *utf8_text;
    size_t utf8_text_length;
} neo_document_t;

/** Base directory: /sdcard/neo when SD is mounted, else /spiflash/neo. */
const char *neo_import_base_dir(void);

/**
 * Build the backup path for a document without writing.
 * Format: {base}/{deviceId}_s{NN}_{file}_{YYYYMMDD}.txt
 */
esp_err_t neo_import_build_document_path(uint8_t file_index, const char *file_name,
                                         char *out_path, size_t out_path_size);

/** True when out_path already exists and its bytes match text. */
bool neo_import_file_matches(const char *path, const char *text, size_t text_len);

/** True when text is empty or only spaces, tabs, CR, or LF. */
bool neo_import_text_is_blank(const char *text, size_t text_len);

/**
 * True when any existing backup in the imports directory already has the same
 * UTF-8 bytes (avoids re-saving identical documents under a new date stamp).
 */
bool neo_import_matching_backup_exists(const char *text, size_t text_len);

/**
 * Delete oldest .txt backups until free space and file-count limits are met.
 * Prefer pruning on internal flash; still runs when SD is the active base.
 * Returns how many files were removed.
 */
size_t neo_import_prune_old_backups(size_t bytes_needed);

/** Save a UTF-8 document atomically into the imports directory.
 *  Returns ESP_ERR_NOT_FOUND if blank, ESP_ERR_INVALID_STATE if duplicate. */
esp_err_t neo_import_save_document(const neo_document_t *document,
                                          char *saved_path, size_t saved_path_size);

/** Save raw NEO bytes into the imports directory; used for raw file downloads. */
esp_err_t neo_import_save_raw_document(const uint8_t *neo_data, size_t neo_len,
                                              const char *file_name, uint8_t file_index,
                                              char *saved_path, size_t saved_path_size);

typedef struct {
    char path[256];
    char name[NEO_DOCUMENT_NAME_MAX_LENGTH + 1];
    uint8_t file_index;
    size_t bytes_saved;
} neo_import_saved_file_t;

/** Read every file on an applet and save non-empty documents locally (NeoTools read-all). */
esp_err_t neo_import_backup_applet_files(uint16_t applet_id, neo_charmap_id_t map,
                                         neo_import_saved_file_t *results, size_t max_results,
                                         size_t *out_count);
