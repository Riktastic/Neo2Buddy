/**
 * @file hid_debug.c
 * @brief In-memory capture of recent Neo HID interrupt reports.
 *
 * Fixed-size ring (64 entries). Each entry stores a timestamp and up to 64 bytes
 * of report data as hex in JSON export. Used by the portal keyboard debug panel
 * and optional serial investigation — not on the hot path for normal typing.
 */

#include "hid_debug.h"

#include "cJSON.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#define HID_DEBUG_CAP 64
#define HID_DEBUG_MAX_REPORT_LEN 64

typedef struct {
    uint64_t ts;
    size_t len;
    uint8_t data[HID_DEBUG_MAX_REPORT_LEN];
} hid_entry_t;

static hid_entry_t entries[HID_DEBUG_CAP];
static size_t head = 0;
static size_t count = 0;

void hid_debug_append(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }
    if (len > HID_DEBUG_MAX_REPORT_LEN) {
        len = HID_DEBUG_MAX_REPORT_LEN;
    }

    size_t idx = (head + count) % HID_DEBUG_CAP;
    if (count == HID_DEBUG_CAP) {
        /* Overwrite oldest entry when full. */
        idx = head;
        head = (head + 1) % HID_DEBUG_CAP;
        count = HID_DEBUG_CAP;
    } else {
        count++;
    }

    entries[idx].ts = (uint64_t)esp_timer_get_time() / 1000000ULL;
    entries[idx].len = len;
    memcpy(entries[idx].data, data, len);
}

esp_err_t hid_debug_get_json(char *out, size_t out_size, int limit)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (limit <= 0) {
        limit = (int)count;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < limit && i < (int)count; ++i) {
        size_t idx = (head + i) % HID_DEBUG_CAP;
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            continue;
        }
        cJSON_AddNumberToObject(o, "ts", (double)entries[idx].ts);

        char hex[HID_DEBUG_MAX_REPORT_LEN * 2 + 1];
        for (size_t j = 0; j < entries[idx].len; ++j) {
            sprintf(&hex[j * 2], "%02x", entries[idx].data[j]);
        }
        hex[entries[idx].len * 2] = '\0';
        cJSON_AddStringToObject(o, "data_hex", hex);
        cJSON_AddItemToArray(arr, o);
    }

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }

    size_t need = strlen(s) + 1;
    if (need > out_size) {
        free(s);
        return ESP_ERR_NO_MEM;
    }
    strcpy(out, s);
    free(s);
    return ESP_OK;
}
