/**
 * @file neo_link_mailbox.c
 * @brief ASM mailbox using NeoTools raw-file semantics (no AlphaWord charmap).
 */

#include "neo_link_mailbox.h"

#include "log_buffer.h"
#include "neo_file.h"
#include "neo_link_text.h"
#include "usb_host_neo.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "neo_link_mb";

static esp_err_t s_last_mailbox_err = ESP_OK;
static SemaphoreHandle_t s_mb_lock;

static bool neo_link_mailbox_lock(void)
{
    if (!s_mb_lock) {
        s_mb_lock = xSemaphoreCreateMutex();
    }
    return s_mb_lock && xSemaphoreTake(s_mb_lock, pdMS_TO_TICKS(30000)) == pdTRUE;
}

static void neo_link_mailbox_unlock(void)
{
    if (s_mb_lock) {
        xSemaphoreGive(s_mb_lock);
    }
}

esp_err_t neo_link_mailbox_last_error(void)
{
    return s_last_mailbox_err;
}

/**
 * NeoTools write for non-AlphaWord: raw bytes.
 * Existing slot → resize attrs if needed then WRITE_RAW (no duplicate create).
 */
static esp_err_t neo_link_mailbox_write_named(const char *name, const uint8_t *data, size_t length)
{
    neo_file_attr_t attrs;
    esp_err_t err = neo_file_find_by_name_or_space(NEO_LINK_APPLET_ID, name, &attrs);
    if (err == ESP_OK) {
        return neo_file_write_raw_resize(NEO_LINK_APPLET_ID, attrs.file_index, data, length);
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    return neo_file_create(NEO_LINK_APPLET_ID, name, "write", data, length);
}

static esp_err_t neo_link_mailbox_deliver_locked(const char *utf8_text, bool return_to_keyboard)
{
    if (!utf8_text || utf8_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Slot size == REPLY_MAX so Neo FileReadBuffer(length) cannot overrun BSS. */
    char *plain = malloc(NEO_LINK_MAILBOX_CAP);
    if (!plain) {
        return ESP_ERR_NO_MEM;
    }
    size_t plain_len = neo_link_text_to_mailbox(utf8_text, plain, NEO_LINK_MAILBOX_CAP);
    if (plain_len == 0) {
        free(plain);
        return ESP_ERR_INVALID_ARG;
    }

    /* Fixed-capacity payload so later replies overwrite without resize pain. */
    uint8_t *slot = calloc(1, NEO_LINK_MAILBOX_CAP);
    if (!slot) {
        free(plain);
        return ESP_ERR_NO_MEM;
    }
    memcpy(slot, plain, plain_len);
    free(plain);

    esp_err_t err = usb_host_neo_ensure_comms();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ensure_comms failed: %s", esp_err_to_name(err));
        free(slot);
        return err;
    }

    err = neo_link_mailbox_write_named(NEO_LINK_MAILBOX_IN_NAME, slot, NEO_LINK_MAILBOX_CAP);
    free(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mailbox write failed: %s", esp_err_to_name(err));
        log_buffer_appendf("neo_link: mailbox write failed %s", esp_err_to_name(err));
        return err;
    }

    log_buffer_appendf("neo_link: delivered %u chars to NeoLinkIn", (unsigned)plain_len);
    ESP_LOGI(TAG, "Delivered %u chars to NeoLinkIn", (unsigned)plain_len);

    if (return_to_keyboard) {
        esp_err_t restart = usb_host_neo_restart();
        if (restart != ESP_OK) {
            ESP_LOGW(TAG, "delivered but keyboard restart failed: %s", esp_err_to_name(restart));
            return restart;
        }
    }
    return ESP_OK;
}

esp_err_t neo_link_mailbox_deliver(const char *utf8_text, bool return_to_keyboard)
{
    if (!neo_link_mailbox_lock()) {
        s_last_mailbox_err = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = neo_link_mailbox_deliver_locked(utf8_text, return_to_keyboard);
    s_last_mailbox_err = err;
    neo_link_mailbox_unlock();
    return err;
}

esp_err_t neo_link_mailbox_fetch_out(char *out, size_t out_size, size_t *out_len, bool return_to_keyboard)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_len) {
        *out_len = 0;
    }
    if (!usb_host_neo_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!neo_link_mailbox_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = usb_host_neo_ensure_comms();
    if (err != ESP_OK) {
        neo_link_mailbox_unlock();
        return err;
    }

    neo_file_attr_t attrs;
    err = neo_file_find_by_name_or_space(NEO_LINK_APPLET_ID, NEO_LINK_MAILBOX_OUT_NAME, &attrs);
    if (err != ESP_OK) {
        /* Try file index 2 (Out) then 1 as last resort naming miss. */
        for (int idx = 2; idx >= 1; idx--) {
            uint8_t *raw = NULL;
            size_t raw_len = 0;
            err = usb_host_neo_read_file_alloc(NEO_LINK_APPLET_ID, (uint8_t)idx, &attrs, &raw, &raw_len,
                                               NEO_LINK_MAILBOX_CAP);
            if (err == ESP_OK && raw && raw_len > 0) {
                size_t n = neo_link_text_to_mailbox((const char *)raw, out, out_size);
                free(raw);
                if (out_len) {
                    *out_len = n;
                }
                if (return_to_keyboard) {
                    (void)usb_host_neo_restart();
                }
                neo_link_mailbox_unlock();
                return n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
            }
            free(raw);
        }
        neo_link_mailbox_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    err = usb_host_neo_read_file_alloc(NEO_LINK_APPLET_ID, attrs.file_index, NULL, &raw, &raw_len,
                                       NEO_LINK_MAILBOX_CAP);
    if (err != ESP_OK || !raw) {
        free(raw);
        neo_link_mailbox_unlock();
        return err != ESP_OK ? err : ESP_FAIL;
    }
    size_t n = neo_link_text_to_mailbox((const char *)raw, out, out_size);
    free(raw);
    if (out_len) {
        *out_len = n;
    }

    if (return_to_keyboard) {
        esp_err_t restart = usb_host_neo_restart();
        if (restart != ESP_OK && n > 0) {
            neo_link_mailbox_unlock();
            return restart;
        }
    }
    neo_link_mailbox_unlock();
    return n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

typedef struct {
    char *text;
    bool return_to_keyboard;
} neo_link_deliver_job_t;

static void neo_link_deliver_task(void *arg)
{
    neo_link_deliver_job_t *job = arg;
    if (job && job->text) {
        neo_link_mailbox_deliver(job->text, job->return_to_keyboard);
        free(job->text);
        free(job);
    }
    vTaskDelete(NULL);
}

void neo_link_mailbox_deliver_async(const char *utf8_text, bool return_to_keyboard)
{
    if (!utf8_text || utf8_text[0] == '\0') {
        return;
    }
    neo_link_deliver_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return;
    }
    job->text = strdup(utf8_text);
    job->return_to_keyboard = return_to_keyboard;
    if (!job->text) {
        free(job);
        return;
    }
    if (xTaskCreate(neo_link_deliver_task, "neo_link_mb", 8192, job, 5, NULL) != pdPASS) {
        free(job->text);
        free(job);
        ESP_LOGW(TAG, "failed to queue mailbox delivery task");
    }
}
