/**
 * @file neo_autobackup.c
 * @brief Auto-backup AlphaWord files on Neo connect, then return to keyboard.
 *
 * Pipeline: settle → usb_host_neo_ensure_comms → files 1..8 (AlphaWord) →
 * optional usb_host_neo_restart. Progress phases feed the portal / Python client.
 *
 * Tried and rejected:
 *   - Triggering from every USB NEW_DEV / monitor tick (flip storms).
 *   - Sync backup inside the USB client task (starves bulk callbacks).
 *   - Always rewriting files (slow; fills SD with duplicates).
 *
 * See firmware/docs/neo-usb-and-backup.md.
 */

#include "neo_autobackup.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "neo_applet.h"
#include "neo_conv.h"
#include "neo_debug.h"
#include "neo_file.h"
#include "neo_import.h"
#include "settings.h"
#include "usb_host_neo.h"
#include "cloud_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "neo_autobackup";

/* After RESTART the Neo reappears as HID; without cooldown that schedules another run. */
#define NEO_AUTOBACKUP_COOLDOWN_MS 120000
/* Give HID claim / bus settle a moment before ensure_comms (async path only). */
#define NEO_AUTOBACKUP_SETTLE_MS 2000
#define NEO_AUTOBACKUP_STACK 8192

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_busy;
static int64_t s_last_done_us;
static bool s_return_to_keyboard = true;
static bool s_force_run; /* CLI/web "now" ignores setting + cooldown */
static esp_err_t s_last_result = ESP_OK;
static neo_autobackup_progress_t s_progress = {
    .busy = false,
    .phase = "idle",
    .current = 0,
    .total = 8,
    .saved = 0,
    .skipped = 0,
};

static void neo_autobackup_set_phase(const char *phase, uint8_t current)
{
    if (phase) {
        strlcpy(s_progress.phase, phase, sizeof(s_progress.phase));
    }
    s_progress.current = current;
    s_progress.busy = s_busy;
}

static esp_err_t neo_autobackup_try_start(bool return_to_keyboard, bool force);

/**
 * Read one AlphaWord slot, convert to UTF-8, skip only when today's backup file
 * for this slot already holds the same bytes. Empty / missing slots increment
 * skipped, not saved.
 */
static esp_err_t backup_file_if_changed(uint16_t applet_id, uint8_t index, size_t *out_saved,
                                        size_t *out_skipped)
{
    neo_file_attr_t attrs;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = usb_host_neo_read_file_alloc(applet_id, index, &attrs, &raw, &raw_len, 256 * 1024);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (raw_len == 0 || neo_import_neo_raw_is_empty(raw, raw_len)) {
        free(raw);
        if (out_skipped) {
            (*out_skipped)++;
        }
        return ESP_OK;
    }

    size_t text_cap = neo_conv_export_buf_size(raw_len);
    char *text = malloc(text_cap);
    if (!text) {
        free(raw);
        return ESP_ERR_NO_MEM;
    }
    size_t text_len = neo_conv_export_text_from_neo(raw, raw_len, text, text_cap, NEO_CHARMAP_EN_US);
    free(raw);
    if (text_len == 0 || neo_import_text_is_blank(text, text_len)) {
        free(text);
        if (out_skipped) {
            (*out_skipped)++;
        }
        return ESP_OK;
    }

    char path[256];
    neo_document_t doc = {
        .file_index = index,
        .file_name = attrs.name,
        .utf8_text = text,
        .utf8_text_length = text_len,
    };
    err = neo_import_save_document_if_changed(&doc, path, sizeof(path));
    free(text);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
        if (out_skipped) {
            (*out_skipped)++;
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (neo_debug_is_verbose()) {
        ESP_LOGI(TAG, "saved index=%u name=%s path=%s", index, attrs.name, path);
    }
    neo_debug_event("autobackup saved index=%u %s", index, attrs.name);
    if (out_saved) {
        (*out_saved)++;
    }
    return ESP_OK;
}

static esp_err_t neo_autobackup_run_internal(bool return_to_keyboard, bool settle)
{
    s_progress.total = 8;
    s_progress.saved = 0;
    s_progress.skipped = 0;

    if (settle) {
        neo_autobackup_set_phase("settle", 0);
        vTaskDelay(pdMS_TO_TICKS(NEO_AUTOBACKUP_SETTLE_MS));
    }

    neo_debug_event("autobackup start return_kb=%d", return_to_keyboard ? 1 : 0);
    ESP_LOGI(TAG, "Auto-backup starting (return_to_keyboard=%d)", return_to_keyboard ? 1 : 0);

    neo_autobackup_set_phase("comms", 0);
    esp_err_t err = usb_host_neo_ensure_comms();
    if (err != ESP_OK) {
        neo_debug_event("autobackup ensure_comms failed: %s", esp_err_to_name(err));
        neo_autobackup_set_phase("idle", 0);
        return err;
    }

    size_t saved = 0;
    size_t skipped = 0;
    for (uint8_t index = 1; index <= 8; index++) {
        neo_autobackup_set_phase("file", index);
        err = backup_file_if_changed(NEO_APPLET_ID_ALPHAWORD, index, &saved, &skipped);
        s_progress.saved = saved;
        s_progress.skipped = skipped;
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err != ESP_OK) {
            neo_debug_event("autobackup file %u failed: %s", index, esp_err_to_name(err));
            ESP_LOGW(TAG, "Auto-backup file %u failed: %s", index, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "Auto-backup done saved=%u skipped=%u", (unsigned)saved, (unsigned)skipped);
    neo_debug_event("autobackup done saved=%u skipped=%u", (unsigned)saved, (unsigned)skipped);

    if (return_to_keyboard) {
        neo_autobackup_set_phase("restart", 0);
        err = usb_host_neo_restart();
        if (err != ESP_OK) {
            neo_debug_event("autobackup restart(keyboard) failed: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "Could not return Neo to keyboard mode: %s", esp_err_to_name(err));
            neo_autobackup_set_phase("idle", 0);
            return err;
        }
        neo_debug_event("autobackup returned Neo to keyboard mode");
        ESP_LOGI(TAG, "Neo returned to keyboard emulation");
    }
    neo_autobackup_set_phase("done", 0);
    return ESP_OK;
}

static void neo_autobackup_maybe_start_cloud_sync(size_t saved_count)
{
    if (saved_count == 0) {
        return;
    }
    device_settings_t s;
    if (settings_load(&s) != ESP_OK || !s.auto_cloud_sync_after_backup) {
        return;
    }
    esp_err_t sync_err = cloud_sync_start_run();
    if (sync_err == ESP_OK) {
        ESP_LOGI(TAG, "Started cloud sync after backup");
        neo_debug_event("autobackup started cloud sync");
    } else if (sync_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Cloud sync after backup failed to start: %s", esp_err_to_name(sync_err));
    }
}

static void neo_autobackup_task(void *arg)
{
    (void)arg;
    bool ret_kb = s_return_to_keyboard;
    esp_err_t err = neo_autobackup_run_internal(ret_kb, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Auto-backup finished with error: %s", esp_err_to_name(err));
    } else {
        neo_autobackup_maybe_start_cloud_sync(s_progress.saved);
    }
    s_last_result = err;
    s_last_done_us = esp_timer_get_time();
    s_busy = false;
    s_force_run = false;
    s_progress.busy = false;
    if (s_progress.phase[0] == '\0' || strcmp(s_progress.phase, "done") != 0) {
        neo_autobackup_set_phase(err == ESP_OK ? "done" : "idle", 0);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static bool neo_autobackup_setting_enabled(void)
{
    device_settings_t s;
    if (settings_load(&s) != ESP_OK) {
        return false;
    }
    return s.auto_backup_on_connect;
}

static esp_err_t neo_autobackup_try_start(bool return_to_keyboard, bool force)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_busy) {
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Auto-backup already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!force && !neo_autobackup_setting_enabled()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!force) {
        int64_t now = esp_timer_get_time();
        if (s_last_done_us > 0 &&
            (now - s_last_done_us) < ((int64_t)NEO_AUTOBACKUP_COOLDOWN_MS * 1000)) {
            ESP_LOGI(TAG, "Auto-backup skipped (cooldown)");
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_busy = true;
    s_force_run = force;
    s_return_to_keyboard = return_to_keyboard;
    s_progress.busy = true;
    s_progress.saved = 0;
    s_progress.skipped = 0;
    neo_autobackup_set_phase("settle", 0);
    xSemaphoreGive(s_lock);

    BaseType_t ok =
        xTaskCreate(neo_autobackup_task, "neo_aback", NEO_AUTOBACKUP_STACK, NULL, 4, &s_task);
    if (ok != pdPASS) {
        s_busy = false;
        s_task = NULL;
        ESP_LOGE(TAG, "Could not start auto-backup task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void neo_autobackup_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void neo_autobackup_on_keyboard_connected(void)
{
    if (!neo_autobackup_setting_enabled()) {
        return;
    }
    neo_debug_event("autobackup scheduled on keyboard connect");
    (void)neo_autobackup_try_start(true, false);
}

bool neo_autobackup_is_busy(void)
{
    return s_busy;
}

void neo_autobackup_get_progress(neo_autobackup_progress_t *out)
{
    if (!out) {
        return;
    }
    *out = s_progress;
    out->busy = s_busy;
}

esp_err_t neo_autobackup_last_result(void)
{
    return s_last_result;
}

esp_err_t neo_autobackup_run_now(bool return_to_keyboard)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Synchronous path for CLI — no settle delay beyond ensure_comms. */
    s_busy = true;
    s_progress.busy = true;
    esp_err_t err = neo_autobackup_run_internal(return_to_keyboard, false);
    s_last_result = err;
    s_last_done_us = esp_timer_get_time();
    s_busy = false;
    s_progress.busy = false;
    if (err == ESP_OK) {
        neo_autobackup_maybe_start_cloud_sync(s_progress.saved);
    }
    return err;
}

esp_err_t neo_autobackup_start_async(bool return_to_keyboard)
{
    return neo_autobackup_try_start(return_to_keyboard, true);
}
