/**
 * @file neo_file.c
 * @brief File attribute parsing and READ/WRITE/CLEAR/COMMIT sequences.
 *
 * See neo_file.h for the 40-byte attribute layout and command flow.
 * All USB traffic goes through neo_device_* after dialogue_start(0x0000).
 */

#include "neo_file.h"
#include <stdlib.h>
#include <string.h>
#include "neo_applet.h"
#include "neo_conv.h"
#include "neo_debug.h"
#include "neo_device.h"
#include "neo_import.h"
#include "neo_message.h"
#include "neo_space.h"
#include "usb_host_neo.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdbool.h>

static const char *TAG = "neo_file";
static const uint8_t s_file_space_codes[] = {0xff, 0x2d, 0x2c, 0x04, 0x0f, 0x0e, 0x0a, 0x01, 0x27};
static esp_err_t neo_file_dialogue_start(void)
{
    return neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
}

int neo_file_space_to_number(uint8_t code)
{
    for (size_t i = 0; i < sizeof(s_file_space_codes); i++) {
        if (s_file_space_codes[i] == code) {
            return (int)i;
        }
    }
    return 0;
}

uint8_t neo_file_space_from_number(int space_number)
{
    if (space_number < 0 || space_number >= (int)sizeof(s_file_space_codes)) {
        return s_file_space_codes[0];
    }
    return s_file_space_codes[space_number];
}

static void neo_file_attr_from_raw(uint8_t file_index, const uint8_t buf[NEO_FILE_ATTR_SIZE], neo_file_attr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->file_index = file_index;
    memcpy(out->name, buf, 15);
    out->name[15] = '\0';
    memcpy(out->password, buf + 0x10, 7);
    out->password[7] = '\0';
    out->min_size = ((uint32_t)buf[0x18] << 24) | ((uint32_t)buf[0x19] << 16) | ((uint32_t)buf[0x1a] << 8) | buf[0x1b];
    out->alloc_size = ((uint32_t)buf[0x1c] << 24) | ((uint32_t)buf[0x1d] << 16) | ((uint32_t)buf[0x1e] << 8) | buf[0x1f];
    out->flags = ((uint32_t)buf[0x20] << 24) | ((uint32_t)buf[0x21] << 16) | ((uint32_t)buf[0x22] << 8) | buf[0x23];
    out->file_space = buf[0x25];
    out->space_number = neo_file_space_to_number(out->file_space);
}

static void neo_file_attr_to_raw(const neo_file_attr_t *in, uint8_t buf[NEO_FILE_ATTR_SIZE])
{
    memset(buf, 0, NEO_FILE_ATTR_SIZE);
    strncpy((char *)buf, in->name, 15);
    strncpy((char *)(buf + 0x10), in->password, 7);
    buf[0x18] = (uint8_t)(in->min_size >> 24);
    buf[0x19] = (uint8_t)(in->min_size >> 16);
    buf[0x1a] = (uint8_t)(in->min_size >> 8);
    buf[0x1b] = (uint8_t)in->min_size;
    buf[0x1c] = (uint8_t)(in->alloc_size >> 24);
    buf[0x1d] = (uint8_t)(in->alloc_size >> 16);
    buf[0x1e] = (uint8_t)(in->alloc_size >> 8);
    buf[0x1f] = (uint8_t)in->alloc_size;
    buf[0x20] = (uint8_t)(in->flags >> 24);
    buf[0x21] = (uint8_t)(in->flags >> 16);
    buf[0x22] = (uint8_t)(in->flags >> 8);
    buf[0x23] = (uint8_t)in->flags;
    buf[0x25] = in->file_space;
}

static esp_err_t neo_file_raw_set_attributes(uint16_t applet_id, uint8_t file_index, const neo_file_attr_t *attrs)
{
    const uint32_t args[][3] = {{file_index, 1, 4}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_SET_FILE_ATTRIBUTES, args);
    esp_err_t err = neo_device_send_command(&request, NEO_RESPONSE_SET_FILE_ATTRIBUTES, NEO_DEFAULT_TIMEOUT_MS, NULL);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t raw[NEO_FILE_ATTR_SIZE];
    neo_file_attr_to_raw(attrs, raw);
    return neo_device_write_extended(raw, sizeof(raw));
}

static esp_err_t neo_file_send_write_raw(uint16_t applet_id, uint8_t file_index, const uint8_t *data, size_t length)
{
    const uint32_t args[][3] = {{file_index, 1, 1}, {(uint32_t)length, 2, 3}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_WRITE_RAW_FILE, args);
    esp_err_t err = neo_device_send_command(&request, NEO_RESPONSE_WRITE_FILE, NEO_DEFAULT_TIMEOUT_MS, NULL);
    if (err != ESP_OK) {
        neo_debug_event("WRITE_RAW_FILE cmd failed: %s", esp_err_to_name(err));
        return err;
    }
    err = neo_device_write_extended(data, length);
    if (err != ESP_OK) {
        return err;
    }
    neo_message_init(&request, NEO_REQUEST_CONFIRM_WRITE_FILE, NULL);
    return neo_device_send_command(&request, NEO_RESPONSE_CONFIRM_WRITE_FILE, NEO_DEFAULT_TIMEOUT_MS, NULL);
}

static esp_err_t neo_file_send_commit(uint16_t applet_id, uint8_t file_index)
{
    const uint32_t args[][3] = {{file_index, 4, 1}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_COMMIT, args);
    return neo_device_send_command(&request, NEO_RESPONSE_COMMIT, NEO_DEFAULT_TIMEOUT_MS, NULL);
}

esp_err_t neo_get_file_attributes(uint16_t applet_id, uint8_t index, neo_file_attr_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[NEO_FILE_ATTR_SIZE];
    esp_err_t err = neo_device_read_file_attributes(applet_id, index, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    neo_file_attr_from_raw(index, buf, out);
    ESP_LOGI(TAG, "File attr %s alloc=%" PRIu32 " min=%" PRIu32, out->name, out->alloc_size, out->min_size);
    return ESP_OK;
}

esp_err_t neo_file_read_raw_with_size(uint16_t applet_id, uint8_t file_index, uint32_t alloc_size,
                                      uint8_t *buffer, size_t capacity, size_t *out_length)
{
    if (!buffer || capacity == 0 || !out_length) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0;
    if (alloc_size == 0) {
        return ESP_OK;
    }
    if (alloc_size > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }
    const uint32_t args[][3] = {{alloc_size, 1, 3}, {file_index, 4, 1}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_READ_RAW_FILE, args);
    err = neo_device_send_command(&request, NEO_RESPONSE_READ_FILE, NEO_DEFAULT_TIMEOUT_MS, NULL);
    if (err == ESP_OK) {
        err = neo_device_read_extended(buffer, capacity, alloc_size, out_length);
    }
    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

static esp_err_t neo_file_send_read_raw_open(uint16_t applet_id, uint8_t file_index, uint32_t alloc_size,
                                             uint8_t *buffer, size_t capacity, size_t *out_length)
{
    if (alloc_size == 0) {
        *out_length = 0;
        return ESP_OK;
    }
    if (alloc_size > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t args[][3] = {{alloc_size, 1, 3}, {file_index, 4, 1}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_READ_RAW_FILE, args);
    esp_err_t err = neo_device_send_command(&request, NEO_RESPONSE_READ_FILE, NEO_DEFAULT_TIMEOUT_MS, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return neo_device_read_extended(buffer, capacity, alloc_size, out_length);
}

esp_err_t neo_file_read_alloc(uint16_t applet_id, uint8_t file_index, neo_file_attr_t *attrs_out,
                              uint8_t **out_data, size_t *out_length, size_t max_bytes)
{
    if (!out_data || !out_length || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_length = 0;

    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    uint8_t attr_buf[NEO_FILE_ATTR_SIZE];
    err = neo_device_get_file_attributes_open(applet_id, file_index, attr_buf, sizeof(attr_buf));
    if (err != ESP_OK) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return err;
    }

    neo_file_attr_t attrs;
    neo_file_attr_from_raw(file_index, attr_buf, &attrs);
    if (attrs_out) {
        *attrs_out = attrs;
    }
    ESP_LOGI(TAG, "File attr %s alloc=%" PRIu32 " min=%" PRIu32, attrs.name, attrs.alloc_size, attrs.min_size);

    /* NeoTools: empty after clear has alloc_size==0. min_size is the reserve floor. */
    if (attrs.alloc_size == 0) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_OK;
    }

    size_t cap = attrs.alloc_size;
    if (cap > max_bytes) {
        cap = max_bytes;
    }
    uint8_t *raw = malloc(cap);
    if (!raw) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    err = neo_file_send_read_raw_open(applet_id, file_index, (uint32_t)cap, raw, cap, &got);
    neo_device_dialogue_end();
    neo_device_unlock();
    if (err != ESP_OK) {
        free(raw);
        return err;
    }
    *out_data = raw;
    *out_length = got;
    return ESP_OK;
}

esp_err_t neo_file_read_raw(uint16_t applet_id, uint8_t file_index, uint8_t *buffer, size_t capacity,
                            size_t *out_length)
{
    if (!buffer || capacity == 0 || !out_length) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0;

    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    uint8_t attr_buf[NEO_FILE_ATTR_SIZE];
    err = neo_device_get_file_attributes_open(applet_id, file_index, attr_buf, sizeof(attr_buf));
    if (err != ESP_OK) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return err;
    }

    neo_file_attr_t attrs;
    neo_file_attr_from_raw(file_index, attr_buf, &attrs);
    ESP_LOGI(TAG, "File attr %s alloc=%" PRIu32 " min=%" PRIu32, attrs.name, attrs.alloc_size, attrs.min_size);

    if (attrs.alloc_size == 0) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_OK;
    }

    /* NeoTools raw_read_file always requests alloc_size (not min_size). */
    err = neo_file_send_read_raw_open(applet_id, file_index, attrs.alloc_size, buffer, capacity, out_length);
    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

/** Trailing AlphaWord pad / unused bytes (NeoTools pads with 0xa7; cleared slots may be 0x00). */
static size_t neo_file_raw_payload_len(const uint8_t *buf, size_t len)
{
    while (len > 0 && (buf[len - 1] == 0xa7 || buf[len - 1] == 0xa4 || buf[len - 1] == 0x00)) {
        len--;
    }
    return len;
}

/**
 * Bytes of user-visible text for list/UI. Pad-only / control-only slots report 0
 * so the portal can hide "empty" 512 B AlphaWord reserves (Read already shows empty).
 */
static size_t neo_file_used_text_len(const uint8_t *raw, size_t raw_len)
{
    if (!raw || raw_len == 0 || neo_import_neo_raw_is_empty(raw, raw_len)) {
        return 0;
    }
    size_t text_cap = neo_conv_export_buf_size(raw_len);
    char *text = malloc(text_cap);
    if (!text) {
        return neo_file_raw_payload_len(raw, raw_len);
    }
    size_t text_len = neo_conv_export_text_from_neo(raw, raw_len, text, text_cap, NEO_CHARMAP_EN_US);
    bool blank = (text_len == 0) || neo_import_text_is_blank(text, text_len);
    free(text);
    return blank ? 0 : text_len;
}

esp_err_t neo_file_write_raw(uint16_t applet_id, uint8_t file_index, const uint8_t *data, size_t length)
{
    if (!data && length != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }
    err = neo_file_send_write_raw(applet_id, file_index, data, length);
    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

esp_err_t neo_file_write_raw_resize(uint16_t applet_id, uint8_t file_index, const uint8_t *data, size_t length)
{
    if (!data && length != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    uint8_t attr_buf[NEO_FILE_ATTR_SIZE];
    err = neo_device_get_file_attributes_open(applet_id, file_index, attr_buf, sizeof(attr_buf));
    if (err != ESP_OK) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return err;
    }

    neo_file_attr_t attrs;
    neo_file_attr_from_raw(file_index, attr_buf, &attrs);
    if (attrs.alloc_size < (uint32_t)length || attrs.min_size < (uint32_t)length) {
        attrs.alloc_size = (uint32_t)length;
        attrs.min_size = (uint32_t)length;
        err = neo_file_raw_set_attributes(applet_id, file_index, &attrs);
        if (err == ESP_OK) {
            err = neo_file_send_commit(applet_id, file_index);
        }
        if (err != ESP_OK) {
            neo_device_dialogue_end();
            neo_device_unlock();
            return err;
        }
    }

    err = neo_file_send_write_raw(applet_id, file_index, data, length);
    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

esp_err_t neo_file_clear(uint16_t applet_id, uint8_t file_index)
{
    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    uint8_t attr_buf[NEO_FILE_ATTR_SIZE];
    err = neo_device_get_file_attributes_open(applet_id, file_index, attr_buf, sizeof(attr_buf));
    if (err != ESP_OK) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return err;
    }

    neo_file_attr_t attrs;
    neo_file_attr_from_raw(file_index, attr_buf, &attrs);
    attrs.alloc_size = 0;
    attrs.min_size = 0;

    err = neo_file_raw_set_attributes(applet_id, file_index, &attrs);
    if (err == ESP_OK) {
        err = neo_file_send_commit(applet_id, file_index);
    }
    if (err == ESP_OK) {
        err = neo_file_send_write_raw(applet_id, file_index, NULL, 0);
    }
    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

esp_err_t neo_file_create(uint16_t applet_id, const char *name, const char *password, const uint8_t *data,
                          size_t length)
{
    if (!name || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_used_space_t usage;
    neo_avail_space_t avail;
    esp_err_t err = neo_space_get_used(applet_id, &usage);
    if (err != ESP_OK) {
        return err;
    }
    err = neo_space_get_available(&avail);
    if (err != ESP_OK) {
        return err;
    }
    if (length + 1024 > avail.free_ram) {
        neo_debug_event("create file: insufficient RAM need=%u free=%u", (unsigned)length, (unsigned)avail.free_ram);
        return ESP_ERR_NO_MEM;
    }
    uint8_t file_index = (uint8_t)(usage.file_count + 1);
    neo_file_attr_t attrs = {0};
    attrs.file_index = file_index;
    strncpy(attrs.name, name, sizeof(attrs.name) - 1);
    if (password) {
        strncpy(attrs.password, password, sizeof(attrs.password) - 1);
    }
    attrs.space_number = 0;
    attrs.file_space = neo_file_space_from_number(0);
    attrs.min_size = (uint32_t)length;
    attrs.alloc_size = (uint32_t)length;
    attrs.flags = 0;
    neo_device_lock();
    err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }
    err = neo_file_raw_set_attributes(applet_id, file_index, &attrs);
    if (err == ESP_OK) {
        err = neo_file_send_commit(applet_id, file_index);
    }
    if (err == ESP_OK) {
        err = neo_file_send_write_raw(applet_id, file_index, data, length);
    }
    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

typedef struct {
    neo_file_attr_t attr;
    size_t used_size;
} neo_file_list_entry_t;

static int neo_file_cmp_list_entry(const void *a, const void *b)
{
    const neo_file_list_entry_t *fa = a;
    const neo_file_list_entry_t *fb = b;
    if (fa->attr.space_number != fb->attr.space_number) {
        return fa->attr.space_number - fb->attr.space_number;
    }
    return strcmp(fa->attr.name, fb->attr.name);
}

esp_err_t neo_file_list_applet(uint16_t applet_id, cJSON *files)
{
    if (!files) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_file_list_entry_t *listed = calloc(32, sizeof(*listed));
    if (!listed) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;

    neo_device_lock();
    esp_err_t err = neo_file_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        free(listed);
        return err;
    }

    for (uint8_t index = 1; index != 0 && count < 32; index++) {
        uint8_t attr_buf[NEO_FILE_ATTR_SIZE];
        err = neo_device_get_file_attributes_open(applet_id, index, attr_buf, sizeof(attr_buf));
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
            break;
        }
        if (err != ESP_OK) {
            neo_device_dialogue_end();
            neo_device_unlock();
            free(listed);
            return err;
        }
        neo_file_attr_from_raw(index, attr_buf, &listed[count].attr);
        listed[count].used_size = 0;

        /* NeoTools: min_size is reserve floor; used text needs a payload peek. */
        uint32_t alloc = listed[count].attr.alloc_size;
        if (alloc > 0 && alloc <= (256 * 1024)) {
            uint8_t *raw = malloc(alloc);
            if (raw) {
                size_t got = 0;
                esp_err_t read_err =
                    neo_file_send_read_raw_open(applet_id, index, alloc, raw, alloc, &got);
                if (read_err == ESP_OK) {
                    /* Match Read/backup semantics: pad-only and export-blank → 0. */
                    listed[count].used_size = neo_file_used_text_len(raw, got);
                } else {
                    ESP_LOGW(TAG, "list used_size peek failed index=%u: %s", index,
                             esp_err_to_name(read_err));
                    /* Do not report alloc as "used" — that made empty 512 B slots look full. */
                    listed[count].used_size = 0;
                }
                free(raw);
            } else {
                listed[count].used_size = 0;
            }
        }
        count++;
    }
    neo_device_dialogue_end();
    neo_device_unlock();

    if (count > 1) {
        qsort(listed, count, sizeof(listed[0]), neo_file_cmp_list_entry);
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            free(listed);
            return ESP_ERR_NO_MEM;
        }
        const neo_file_attr_t *a = &listed[i].attr;
        cJSON_AddStringToObject(item, "name", a->name);
        cJSON_AddNumberToObject(item, "applet_id", applet_id);
        cJSON_AddNumberToObject(item, "file_index", a->file_index);
        cJSON_AddNumberToObject(item, "index", a->file_index);
        cJSON_AddNumberToObject(item, "alloc_size", a->alloc_size);
        cJSON_AddNumberToObject(item, "min_size", a->min_size);
        cJSON_AddNumberToObject(item, "used_size", (double)listed[i].used_size);
        cJSON_AddNumberToObject(item, "space", a->space_number);
        cJSON_AddNumberToObject(item, "flags", a->flags);
        cJSON_AddItemToArray(files, item);
    }
    free(listed);
    return ESP_OK;
}

