/**
 * @file neo_live.c
 * @brief Thread-safe bounded buffer for recent typed text.
 *
 * `neo_live` provides a small, shared buffer that higher-level code can
 * append to (for example when the device receives typed characters from a
 * connected NEO device) and that the web UI can snapshot and display.
 *
 * Design notes:
 * - The buffer is bounded (NEO_LIVE_TEXT_CAPACITY) and behaves as a rolling
 *   window: older text is dropped when capacity is reached.
 * - A lightweight mutex protects concurrent access from ISR/Tasks.
 * - A monotonically increasing `text_sequence` allows clients to detect
 *   whether the snapshot they read is fresh.
 */

#include "neo_live.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "neo_live";

#define NEO_LIVE_TEXT_CAPACITY 2048

static char text[NEO_LIVE_TEXT_CAPACITY];
static size_t text_length;
static unsigned long text_sequence;
static SemaphoreHandle_t text_mutex;
static bool s_key_log_enabled = false;

void neo_live_init(void)
{
    if (text_mutex == NULL) text_mutex = xSemaphoreCreateMutex();
    /* Start with an empty buffer and increment the sequence. */
    neo_live_clear();
}

void neo_live_set_key_log(bool enabled)
{
    s_key_log_enabled = enabled;
}

bool neo_live_get_key_log(void)
{
    return s_key_log_enabled;
}

/** Append `length` bytes from `input` into the rolling buffer.
 *  If the incoming block exceeds capacity, only the tail is stored. */
void neo_live_append(const char *input, size_t length)
{
    if (!input || length == 0 || !text_mutex) return;
    if (xSemaphoreTake(text_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    /* If incoming chunk alone exceeds capacity, keep only the tail. */
    if (length >= NEO_LIVE_TEXT_CAPACITY) {
        input += length - (NEO_LIVE_TEXT_CAPACITY - 1);
        length = NEO_LIVE_TEXT_CAPACITY - 1;
        text_length = 0;
    }
    /* If adding would overflow, drop the oldest bytes to make room. */
    if (text_length + length >= NEO_LIVE_TEXT_CAPACITY) {
        size_t drop = text_length + length - (NEO_LIVE_TEXT_CAPACITY - 1);
        memmove(text, text + drop, text_length - drop);
        text_length -= drop;
    }
    memcpy(text + text_length, input, length);
    text_length += length;
    text[text_length] = '\0';
    text_sequence++;

    /* Optional UART mirror — off by default; enable via `keyboard keylog on`. */
    if (s_key_log_enabled) {
        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char)input[i];
            if (c == '\n') {
                ESP_LOGI(TAG, "key: \\n");
            } else if (c == '\r') {
                ESP_LOGI(TAG, "key: \\r");
            } else if (c == '\t') {
                ESP_LOGI(TAG, "key: \\t");
            } else if (isprint(c)) {
                ESP_LOGI(TAG, "key: %c", c);
            } else {
                ESP_LOGI(TAG, "key: 0x%02x", c);
            }
        }
    }

    xSemaphoreGive(text_mutex);
}

/** Clear the buffer and bump the sequence to signal consumers. */
void neo_live_clear(void)
{
    if (!text_mutex || xSemaphoreTake(text_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    text[0] = '\0';
    text_length = 0;
    text_sequence++;
    xSemaphoreGive(text_mutex);
}

esp_err_t neo_live_get_sequence(unsigned long *sequence)
{
    if (!sequence || !text_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(text_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *sequence = text_sequence;
    xSemaphoreGive(text_mutex);
    return ESP_OK;
}

/**
 * Snapshot the current buffer into `buffer` and return the current sequence.
 * Returns ESP_ERR_INVALID_SIZE if the provided buffer is too small.
 */
esp_err_t neo_live_snapshot(char *buffer, size_t buffer_size, unsigned long *sequence)
{
    if (!buffer || buffer_size == 0 || !sequence || !text_mutex) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(text_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (text_length >= buffer_size) {
        xSemaphoreGive(text_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, text, text_length + 1);
    *sequence = text_sequence;
    xSemaphoreGive(text_mutex);
    return ESP_OK;
}

esp_err_t neo_live_tail(char *buffer, size_t buffer_size, unsigned long *sequence)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';
    if (!text_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(text_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t out_cap = buffer_size - 1;
    size_t start = 0;
    if (text_length > out_cap) {
        start = text_length - out_cap;
    }
    size_t n = text_length - start;
    for (size_t i = 0; i < n; ++i) {
        char c = text[start + i];
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        } else if ((unsigned char)c < 32 || (unsigned char)c > 126) {
            c = '?';
        }
        buffer[i] = c;
    }
    buffer[n] = '\0';
    if (sequence) {
        *sequence = text_sequence;
    }
    xSemaphoreGive(text_mutex);
    return ESP_OK;
}
