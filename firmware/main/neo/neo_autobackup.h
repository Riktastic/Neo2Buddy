/**
 * @file neo_autobackup.h
 * @brief Optional auto-backup of AlphaWord files when Neo connects.
 *
 * Product flow (why this exists):
 *   HID plug-in → flip to comms → save *changed* AlphaWord files → RESTART to
 *   keyboard so the user can keep typing. Leaving Neo in manager mode after a
 *   quiet plug-in felt broken in testing.
 *
 * Why "changed" instead of always read-all:
 *   Full dumps on every connect are slow and rewrite identical files. We compare
 *   UTF-8 export to the dated local path (neo_import_file_matches). Explicit
 *   "Backup all" stays a separate API for intentional full copies.
 *
 * Why cooldown + async:
 *   RESTART re-enumerates as HID and would retrigger forever without a cooldown.
 *   File I/O must not run on the USB client task — use the worker for connect /
 *   web; CLI may call the blocking run_now path.
 *
 * Design notes: firmware/docs/neo-usb-and-backup.md
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool busy;
    char phase[16]; /**< idle|settle|comms|file|restart|done */
    uint8_t current; /**< 1-based file index while phase=file */
    uint8_t total;
    size_t saved;
    size_t skipped;
} neo_autobackup_progress_t;

/** Call once from app startup (idempotent). */
void neo_autobackup_init(void);

/**
 * Notify that Neo appeared in keyboard mode (HID). May schedule a background
 * auto-backup if the setting is enabled.
 */
void neo_autobackup_on_keyboard_connected(void);

/** True while an auto-backup worker is running. */
bool neo_autobackup_is_busy(void);

/** Snapshot of in-progress or last completed backup state. */
void neo_autobackup_get_progress(neo_autobackup_progress_t *out);

/** Last finished run result (ESP_OK if none yet or still running). */
esp_err_t neo_autobackup_last_result(void);

/**
 * Run backup now (ensure_comms → changed files → restart to keyboard).
 * Blocks the calling task; intended for CLI / explicit UI actions.
 */
esp_err_t neo_autobackup_run_now(bool return_to_keyboard);

/**
 * Start backup on a background task (non-blocking). Returns ESP_ERR_INVALID_STATE
 * if already busy. Intended for the web UI.
 */
esp_err_t neo_autobackup_start_async(bool return_to_keyboard);
