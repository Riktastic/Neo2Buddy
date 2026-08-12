/**
 * @file file_manager.h
 * @brief Local backup file CRUD on SD or SPIFFS (/api/v1/files).
 *
 * Complements neo_import (which builds dated Neo backup paths). This API uses
 * simple names chosen by the user for upload/download/delete. Names are
 * sanitized; paths never escape the storage root.
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

#define FILE_MANAGER_NAME_MAX 64
#define FILE_MANAGER_MAX_UPLOAD (256 * 1024)

/** Active storage root (SD preferred, SPIFFS fallback). */
const char *file_manager_base_path(void);
/** True when the littlefs/SPIFFS portal partition is mounted and usable. */
bool file_manager_flash_ready(void);
/**
 * End-to-end probe: ensure dir, write a small backup file, list it, read it back, delete.
 * @param detail optional human-readable result (OK reason or error).
 */
esp_err_t file_manager_probe_backup_storage(char *detail, size_t detail_size);
/** Ensure the storage directory exists. */
esp_err_t file_manager_ensure_dir(void);
/** Append file entries (name, size, modified) to a JSON array. */
esp_err_t file_manager_list(cJSON *array);
/** Reject unsafe or empty names. */
esp_err_t file_manager_validate_name(const char *name);
/** Resolve a safe absolute path for a file name. */
esp_err_t file_manager_resolve_path(const char *name, char *out, size_t out_size);
/** Bytes available for new uploads. */
size_t file_manager_free_bytes(void);
/** Write or replace a text file. */
esp_err_t file_manager_upload(const char *name, const uint8_t *data, size_t length);
/** Delete a file; requires confirm=true semantics at the API layer. */
esp_err_t file_manager_delete(const char *name);
/** Rename within the storage directory. */
esp_err_t file_manager_rename(const char *old_name, const char *new_name);
