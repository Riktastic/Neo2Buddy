/**
 * @file log_buffer.c
 * @brief Ring buffer implementation for portal-visible logs.
 *
 * 64 lines × 256 chars, circular overwrite. Timestamps come from esp_timer
 * (milliseconds). The web API reads via log_buffer_get_recent_json(); neo_debug
 * optionally mirrors protocol traces here when verbose mode is on.
 */

#include "log_buffer.h"
#include "esp_log.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "log_buffer";

#define LOG_BUFFER_LINES 64
#define LOG_LINE_MAX 256

typedef struct {
    uint64_t ts_ms;
    log_level_t level;
    char msg[LOG_LINE_MAX];
} log_entry_t;

static log_entry_t s_entries[LOG_BUFFER_LINES];
static int s_head = 0; // next write index
static int s_count = 0;
static bool s_init = false;
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t log_buffer_init(void)
{
    if (s_init) return ESP_OK;
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0; s_count = 0; s_init = true;
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "log buffer initialized (lines=%d, len=%d)", LOG_BUFFER_LINES, LOG_LINE_MAX);
    return ESP_OK;
}

void log_buffer_appendf(const char *fmt, ...)
{
    if (!s_init) log_buffer_init();
    va_list ap;
    va_start(ap, fmt);
    char tmp[LOG_LINE_MAX];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    // Delegate to level-aware append with INFO level
    log_buffer_append_level(LOG_LEVEL_INFO, "%s", tmp);
}

void log_buffer_append_level(log_level_t level, const char *fmt, ...)
{
    if (!s_init) log_buffer_init();
    va_list ap;
    va_start(ap, fmt);
    char tmp[LOG_LINE_MAX];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    uint64_t ts_us = esp_timer_get_time();
    uint64_t ts_ms = ts_us / 1000ULL;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return; /* drop log under contention rather than stall callers */
    }
    s_entries[s_head].ts_ms = ts_ms;
    s_entries[s_head].level = level;
    strncpy(s_entries[s_head].msg, tmp, LOG_LINE_MAX-1);
    s_entries[s_head].msg[LOG_LINE_MAX-1] = '\0';
    s_head = (s_head + 1) % LOG_BUFFER_LINES;
    if (s_count < LOG_BUFFER_LINES) s_count++;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

esp_err_t log_buffer_get_recent(char *out, size_t out_len, int max_lines)
{
    if (!out || out_len == 0 || max_lines <= 0) return ESP_ERR_INVALID_ARG;
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int lines_available = s_count;
    int lines_to_return = lines_available < max_lines ? lines_available : max_lines;
    int idx = (s_head - 1 + LOG_BUFFER_LINES) % LOG_BUFFER_LINES;
    size_t pos = 0;
    for (int i = 0; i < lines_to_return; ++i) {
        log_entry_t *e = &s_entries[idx];
        int written = snprintf(out + pos, out_len - pos, "%llu %s %s\n",
                               (unsigned long long)e->ts_ms,
                               (e->level == LOG_LEVEL_DEBUG) ? "DEBUG" :
                               (e->level == LOG_LEVEL_INFO) ? "INFO" :
                               (e->level == LOG_LEVEL_WARN) ? "WARN" : "ERROR",
                               e->msg);
        if (written < 0 || (size_t)written >= out_len - pos) break;
        pos += (size_t)written;
        idx = (idx - 1 + LOG_BUFFER_LINES) % LOG_BUFFER_LINES;
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t log_buffer_get_recent_json(char *out, size_t out_len, int max_lines, log_level_t min_level)
{
    if (!out || out_len == 0 || max_lines <= 0) return ESP_ERR_INVALID_ARG;
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int lines_available = s_count;
    int idx = (s_head - 1 + LOG_BUFFER_LINES) % LOG_BUFFER_LINES;
    cJSON *arr = cJSON_CreateArray();
    int added = 0;
    for (int i = 0; i < lines_available && added < max_lines; ++i) {
        log_entry_t *e = &s_entries[idx];
        if (e->level >= min_level) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "ts_ms", (double)e->ts_ms);
            const char *lvl = (e->level == LOG_LEVEL_DEBUG) ? "DEBUG" :
                              (e->level == LOG_LEVEL_INFO) ? "INFO" :
                              (e->level == LOG_LEVEL_WARN) ? "WARN" : "ERROR";
            cJSON_AddStringToObject(obj, "level", lvl);
            cJSON_AddStringToObject(obj, "msg", e->msg);
            cJSON_AddItemToArray(arr, obj);
            added++;
        }
        idx = (idx - 1 + LOG_BUFFER_LINES) % LOG_BUFFER_LINES;
    }
    // arr currently has newest-first order as we iterated from head-1 downwards
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) {
        if (s_mutex) xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    size_t jlen = strlen(json);
    if (jlen + 1 > out_len) {
        // not enough space
        free(json);
        if (s_mutex) xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, json, jlen+1);
    free(json);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ESP_OK;
}
