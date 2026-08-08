/**
 * @file sd_format.h
 * @brief Async FAT format of the microSD card.
 *
 * POST /api/v1/sd/format starts a background worker so the HTTP handler returns
 * immediately. Poll sd_format_get_status() or GET /api/v1/sd/status for progress.
 * Destructive — all files on the card are erased. Neo backups on SD are lost;
 * spiflash copies (if any) are unaffected.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Start format task. Returns ESP_ERR_INVALID_STATE if already formatting. */
esp_err_t sd_format_start(void);

/** Query in-progress state and 0..100 progress estimate. */
void sd_format_get_status(bool *formatting, int *progress_percent);
