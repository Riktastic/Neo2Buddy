/**
 * @file hammer_ink.h
 * @brief Minimal Hammer Editor (hammer.ink) sync client for Neo backup upload.
 *
 * Implements enough of the Hammer sync protocol (v3) to:
 *   1. Login with email/password
 *   2. Ensure a project exists (default name from config)
 *   3. Upload each local .txt backup as a Note entity (force overwrite)
 *
 * Local files remain authoritative. This is a one-way backup into Hammer Notes,
 * not a full bidirectional Hammer client.
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAMMER_INK_DEFAULT_ENDPOINT "https://hammer.ink"
#define HAMMER_INK_DEFAULT_PROJECT "Neo2 Buddy"
#define HAMMER_INK_PROTOCOL_VERSION 3

typedef struct {
    const char *endpoint;     /**< e.g. https://hammer.ink (no trailing slash) */
    const char *email;
    const char *password;
    const char *project_name; /**< Hammer project name; NULL → Neo2 Buddy */
} hammer_ink_config_t;

typedef void (*hammer_ink_progress_cb_t)(uint8_t current, uint8_t total, const char *filename, void *ctx);

/** Login + account begin/end sync. Does not upload files. */
esp_err_t hammer_ink_test(const hammer_ink_config_t *cfg, char *message, size_t message_size);

/**
 * Upload all local *.txt backups as Notes in the Hammer project.
 * Creates the project on first use. Maps filenames → entity IDs persistently.
 */
esp_err_t hammer_ink_upload_backups(const hammer_ink_config_t *cfg, uint32_t *uploaded, uint32_t *failed,
                                    hammer_ink_progress_cb_t progress, void *progress_ctx, char *err,
                                    size_t err_size);

#ifdef __cplusplus
}
#endif
