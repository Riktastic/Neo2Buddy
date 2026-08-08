/**
 * @file sd_format.c
 * @brief Asynchronous SD card formatting helper with progress reporting.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sd_card.h"
#include "sd_format.h"

static const char *TAG = "sd_format";
static SemaphoreHandle_t s_lock = NULL;
static bool s_formatting = false;
static int s_progress = 0;
static TaskHandle_t s_task = NULL;

static void sd_format_task(void *arg)
{
    (void)arg;

    s_progress = 10;
    vTaskDelay(pdMS_TO_TICKS(200));

    s_progress = 30;
    esp_err_t result = sd_card_mount_formatted();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SD format failed: %s", esp_err_to_name(result));
        goto fail;
    }

    s_progress = 100;
    vTaskDelay(pdMS_TO_TICKS(400));
    s_formatting = false;
    s_task = NULL;
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    ESP_LOGI(TAG, "SD format completed");
    vTaskDelete(NULL);
    return;

fail:
    s_formatting = false;
    s_progress = 0;
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    s_task = NULL;
    ESP_LOGW(TAG, "SD format failed");
    vTaskDelete(NULL);
}

esp_err_t sd_format_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lock, (TickType_t)0) != pdTRUE) {
        return ESP_FAIL;
    }
    if (s_formatting) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    s_formatting = true;
    s_progress = 0;
    xSemaphoreGive(s_lock);

    BaseType_t rc = xTaskCreate(sd_format_task, "sd_format", 8192, NULL, tskIDLE_PRIORITY + 3, &s_task);
    if (rc != pdPASS) {
        s_formatting = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sd_format_get_status(bool *formatting, int *progress_percent)
{
    if (formatting) {
        *formatting = s_formatting;
    }
    if (progress_percent) {
        *progress_percent = s_progress;
    }
}
