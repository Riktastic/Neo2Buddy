/**
 * @file neo_debug.c
 * @brief In-memory Neo USB / protocol diagnostic trace.
 *
 * Design choice: always record into the ring (field forensics via `neo debug`
 * / GET /api/v1/neo/debug), but mirror to ESP_LOG / portal log_buffer only when
 * verbose. Dumping every ensure_comms success made serial unusable.
 *
 * The ring is protected only by a short portMUX critical section so producers
 * never block on a FreeRTOS mutex (which previously caused Interrupt WDT
 * timeouts under multi-task USB/console/monitor contention).
 */

#include "neo_debug.h"

#include "log_buffer.h"
#include "neo_message.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "neo_dbg";
static const char *PROTO_TAG = "neo_proto";

#define NEO_DEBUG_CAP 64
#define NEO_DEBUG_DETAIL 160
#define NEO_DEBUG_HEX_BYTES 8

typedef struct {
    uint64_t ts_ms;
    char tag[16];
    char detail[NEO_DEBUG_DETAIL];
    char hex[(NEO_DEBUG_HEX_BYTES * 2) + 1];
} neo_debug_entry_t;

static neo_debug_entry_t s_entries[NEO_DEBUG_CAP];
static size_t s_head = 0;
static size_t s_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_verbose = false;

void neo_debug_init(void)
{
    /* Ring is static BSS; nothing else to set up. */
}

void neo_debug_set_verbose(bool enabled)
{
    s_verbose = enabled;
}

bool neo_debug_is_verbose(void)
{
    return s_verbose;
}

static void neo_debug_mirror_info(const char *tag, const char *detail)
{
    if (!s_verbose || !detail) {
        return;
    }
    ESP_LOGI(tag, "%s", detail);
    log_buffer_appendf("neo: %s", detail);
}

static void neo_debug_mirror_warn(const char *tag, const char *detail)
{
    if (!detail) {
        return;
    }
    /* Failures always surface on serial and in the portal log buffer. */
    ESP_LOGW(tag, "%s", detail);
    log_buffer_appendf("neo: %s", detail);
}

static void neo_debug_push_locked(const char *tag, const char *detail, const uint8_t *data, size_t len)
{
    if (!tag) {
        tag = "event";
    }
    if (!detail) {
        detail = "";
    }

    size_t idx;
    if (s_count < NEO_DEBUG_CAP) {
        idx = (s_head + s_count) % NEO_DEBUG_CAP;
        s_count++;
    } else {
        idx = s_head;
        s_head = (s_head + 1) % NEO_DEBUG_CAP;
    }

    neo_debug_entry_t *e = &s_entries[idx];
    e->ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    strncpy(e->tag, tag, sizeof(e->tag) - 1);
    e->tag[sizeof(e->tag) - 1] = '\0';
    strncpy(e->detail, detail, sizeof(e->detail) - 1);
    e->detail[sizeof(e->detail) - 1] = '\0';
    e->hex[0] = '\0';

    if (data != NULL && len > 0) {
        size_t n = len > NEO_DEBUG_HEX_BYTES ? NEO_DEBUG_HEX_BYTES : len;
        for (size_t i = 0; i < n; i++) {
            sprintf(&e->hex[i * 2], "%02x", data[i]);
        }
        e->hex[n * 2] = '\0';
    }
}

static void neo_debug_push(const char *tag, const char *detail, const uint8_t *data, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    neo_debug_push_locked(tag, detail, data, len);
    portEXIT_CRITICAL(&s_mux);
}

static void format_bytes(char *out, size_t out_size, const uint8_t *data, size_t len)
{
    if (!out || out_size == 0 || !data) {
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < out_size; i++) {
        pos += (size_t)snprintf(out + pos, out_size - pos, "%s%02x", i ? " " : "", data[i]);
    }
}

static void format_protocol_line(char *detail, size_t detail_size, const char *arrow, const char *label,
                                 const uint8_t data[8], bool checksum_bad)
{
    const char *name = neo_message_command_name(data[0]);
    neo_message_t tmp;
    memcpy(tmp.data, data, 8);
    uint32_t arg32 = neo_message_argument(&tmp, 1, 4);
    uint16_t arg16 = (uint16_t)neo_message_argument(&tmp, 5, 2);

    char bytes[32];
    format_bytes(bytes, sizeof(bytes), data, 8);

    snprintf(detail, detail_size, "%s %s %s (0x%02x): [%s] arg32=%lu arg16=%u%s", arrow, label, name, data[0],
             bytes, (unsigned long)arg32, (unsigned)arg16, checksum_bad ? " CHECKSUM_BAD" : "");
}

void neo_debug_event(const char *fmt, ...)
{
    char detail[NEO_DEBUG_DETAIL];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    neo_debug_mirror_info(TAG, detail);
    neo_debug_push("event", detail, NULL, 0);
}

void neo_debug_xfer(const char *direction, esp_err_t err, const uint8_t *data, size_t len)
{
    char detail[NEO_DEBUG_DETAIL];
    const char *err_name = err == ESP_OK ? "OK" : esp_err_to_name(err);
    if (len > 0) {
        snprintf(detail, sizeof(detail), "%s err=%s len=%u", direction ? direction : "usb", err_name,
                 (unsigned)len);
    } else {
        snprintf(detail, sizeof(detail), "%s err=%s", direction ? direction : "usb", err_name);
    }

    /* Successful bulk xfers stay ring-only. Failures always warn on serial. */
    if (err != ESP_OK) {
        neo_debug_mirror_warn(TAG, detail);
        if (data != NULL && len > 0) {
            size_t hex_len = len > NEO_DEBUG_HEX_BYTES ? NEO_DEBUG_HEX_BYTES : len;
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, hex_len, ESP_LOG_WARN);
        }
    } else if (s_verbose) {
        neo_debug_mirror_info(TAG, detail);
    }

    neo_debug_push(direction ? direction : "usb", detail, data, len);
}

void neo_debug_message(const char *direction, const uint8_t data[8])
{
    if (!data) {
        return;
    }

    bool checksum_bad =
        data[7] != (uint8_t)((data[0] + data[1] + data[2] + data[3] + data[4] + data[5] + data[6]) & 0xFF);

    const char *label = "FRAME";
    const char *arrow = ">>>";

    if (direction && (strcmp(direction, "msg_in") == 0 || strcmp(direction, "RESPONSE") == 0)) {
        label = "RESPONSE";
        arrow = "<<<";
    } else if (direction && (strcmp(direction, "msg_out") == 0 || strcmp(direction, "REQUEST") == 0)) {
        label = "REQUEST";
        arrow = ">>>";
    } else if (direction && strcmp(direction, "msg_in_bad") == 0) {
        label = "RESPONSE";
        arrow = "<<<";
        checksum_bad = true;
    }

    char detail[NEO_DEBUG_DETAIL];
    format_protocol_line(detail, sizeof(detail), arrow, label, data, checksum_bad);

    if (checksum_bad) {
        neo_debug_mirror_warn(PROTO_TAG, detail);
    } else {
        neo_debug_mirror_info(PROTO_TAG, detail);
    }
    neo_debug_push(label, detail, data, 8);
}

void neo_debug_raw(const char *label, const char *direction, const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }

    const char *arrow = (direction && strcmp(direction, "RESPONSE") == 0) ? "<<<" : ">>>";
    char bytes[96];
    format_bytes(bytes, sizeof(bytes), data, len > 32 ? 32 : len);

    char detail[NEO_DEBUG_DETAIL];
    snprintf(detail, sizeof(detail), "%s %s %s (%u bytes): [%s]%s", arrow, direction ? direction : "FRAME",
             label ? label : "data", (unsigned)len, bytes, len > 32 ? "..." : "");

    neo_debug_mirror_info(PROTO_TAG, detail);
    neo_debug_push(label ? label : "raw", detail, data, len);
}

void neo_debug_command_exchange(const uint8_t request[8], const uint8_t response[8], uint8_t expected_response,
                                esp_err_t result)
{
    if (!request || !response) {
        return;
    }

    const char *req_name = neo_message_command_name(request[0]);
    const char *rsp_name = neo_message_command_name(response[0]);
    char req_bytes[32];
    char rsp_bytes[32];
    format_bytes(req_bytes, sizeof(req_bytes), request, 8);
    format_bytes(rsp_bytes, sizeof(rsp_bytes), response, 8);

    neo_message_t req_msg;
    neo_message_t rsp_msg;
    memcpy(req_msg.data, request, 8);
    memcpy(rsp_msg.data, response, 8);
    uint32_t rsp_arg32 = neo_message_argument(&rsp_msg, 1, 4);
    uint16_t rsp_arg16 = (uint16_t)neo_message_argument(&rsp_msg, 5, 2);

    char detail[NEO_DEBUG_DETAIL];
    if (result == ESP_OK) {
        snprintf(detail, sizeof(detail),
                 "EXCHANGE OK: %s (0x%02x) [%s] -> %s (0x%02x) [%s] rsp_arg32=%lu rsp_arg16=%u", req_name,
                 request[0], req_bytes, rsp_name, response[0], rsp_bytes, (unsigned long)rsp_arg32,
                 (unsigned)rsp_arg16);
        neo_debug_mirror_info(PROTO_TAG, detail);
    } else {
        const char *expect_name = expected_response ? neo_message_command_name(expected_response) : "any";
        snprintf(detail, sizeof(detail),
                 "EXCHANGE FAIL: %s (0x%02x) [%s] -> %s (0x%02x) [%s] expected=%s err=%s", req_name,
                 request[0], req_bytes, rsp_name, response[0], rsp_bytes, expect_name, esp_err_to_name(result));
        neo_debug_mirror_warn(PROTO_TAG, detail);
    }

    neo_debug_push("exchange", detail, response, 8);
}

esp_err_t neo_debug_get_json(char *out, size_t out_size, int limit)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Allocate outside the critical section — never malloc while holding s_mux. */
    neo_debug_entry_t *snapshot = calloc(NEO_DEBUG_CAP, sizeof(neo_debug_entry_t));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }

    size_t snapshot_count = 0;
    portENTER_CRITICAL(&s_mux);
    int use_limit = limit;
    if (use_limit <= 0 || use_limit > (int)s_count) {
        use_limit = (int)s_count;
    }
    if (use_limit > NEO_DEBUG_CAP) {
        use_limit = NEO_DEBUG_CAP;
    }
    for (int i = 0; i < use_limit; i++) {
        size_t idx = (s_head + (size_t)i) % NEO_DEBUG_CAP;
        snapshot[i] = s_entries[idx];
    }
    snapshot_count = (size_t)use_limit;
    portEXIT_CRITICAL(&s_mux);

    if (snapshot_count == 0) {
        free(snapshot);
        strncpy(out, "[]", out_size);
        out[out_size - 1] = '\0';
        return ESP_OK;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        free(snapshot);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < snapshot_count; i++) {
        const neo_debug_entry_t *e = &snapshot[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            continue;
        }
        cJSON_AddNumberToObject(o, "ts_ms", (double)e->ts_ms);
        cJSON_AddStringToObject(o, "tag", e->tag);
        cJSON_AddStringToObject(o, "detail", e->detail);
        if (e->hex[0] != '\0') {
            cJSON_AddStringToObject(o, "hex", e->hex);
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(snapshot);

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(out, s, out_size - 1);
    out[out_size - 1] = '\0';
    cJSON_free(s);
    return ESP_OK;
}
