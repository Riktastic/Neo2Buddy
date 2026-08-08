/**
 * @file neo_settings.c
 * @brief Parse and emit SmartApplet settings binary (GET/SET_SETTINGS).
 *
 * Settings items are length-prefixed records: type (u16), ident (u16), length (u16),
 * payload. We convert to JSON for the web API and back for SET. Dialogue uses the
 * target applet id (not always system).
 */

#include "neo_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neo_applet.h"
#include "neo_debug.h"
#include "neo_device.h"
#include "neo_message.h"

enum {
    NEO_SETTINGS_TYPE_NONE = 0x0000,
    NEO_SETTINGS_TYPE_LABEL = 0x0001,
    NEO_SETTINGS_TYPE_RANGE32 = 0x0102,
    NEO_SETTINGS_TYPE_OPTION = 0x0103,
    NEO_SETTINGS_TYPE_PASSWORD6 = 0x0105,
    NEO_SETTINGS_TYPE_DESCRIPTION = 0x0106,
    NEO_SETTINGS_TYPE_FILE_PASSWORD = 0xc001,
    NEO_SETTINGS_TYPE_APPLET_ID = 0x8002,
};

static uint16_t read_u16_be(const uint8_t *buf, size_t offset)
{
    return (uint16_t)(((uint16_t)buf[offset] << 8) | buf[offset + 1]);
}

static uint32_t read_u32_be(const uint8_t *buf, size_t offset)
{
    return ((uint32_t)buf[offset] << 24) | ((uint32_t)buf[offset + 1] << 16) | ((uint32_t)buf[offset + 2] << 8) |
           buf[offset + 3];
}

static cJSON *neo_settings_parse_item(const uint8_t *buf, size_t total_len)
{
    if (total_len < 6) {
        return NULL;
    }
    uint16_t type = read_u16_be(buf, 0);
    uint16_t ident = read_u16_be(buf, 2);
    uint16_t length = read_u16_be(buf, 4);
    if (type == 0 && ident == 0 && length == 0) {
        return NULL;
    }

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return NULL;
    }
    cJSON_AddNumberToObject(item, "ident", ident);
    cJSON_AddNumberToObject(item, "type", type);

    const uint8_t *data = buf + 6;
    switch (type) {
    case NEO_SETTINGS_TYPE_RANGE32:
        if (length >= 12) {
            cJSON *range = cJSON_CreateObject();
            cJSON_AddNumberToObject(range, "default", read_u32_be(data, 0));
            cJSON_AddNumberToObject(range, "min", read_u32_be(data, 4));
            cJSON_AddNumberToObject(range, "max", read_u32_be(data, 8));
            cJSON_AddItemToObject(item, "value", range);
        }
        break;
    case NEO_SETTINGS_TYPE_OPTION: {
        cJSON *options = cJSON_CreateArray();
        for (size_t offset = 0; offset + 1 < length; offset += 2) {
            cJSON_AddItemToArray(options, cJSON_CreateNumber(read_u16_be(data, offset)));
        }
        cJSON_AddItemToObject(item, "value", options);
        break;
    }
    case NEO_SETTINGS_TYPE_APPLET_ID:
        if (length >= 4) {
            cJSON_AddNumberToObject(item, "value", read_u32_be(data, 0));
        }
        break;
    case NEO_SETTINGS_TYPE_LABEL:
    case NEO_SETTINGS_TYPE_DESCRIPTION:
    case NEO_SETTINGS_TYPE_PASSWORD6:
    case NEO_SETTINGS_TYPE_FILE_PASSWORD:
        cJSON_AddStringToObject(item, "value", (const char *)data);
        break;
    default: {
        char hex[64];
        size_t hex_len = length < 16 ? length : 16;
        size_t pos = 0;
        for (size_t i = 0; i < hex_len && pos + 3 < sizeof(hex); ++i) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02x", data[i]);
        }
        cJSON_AddStringToObject(item, "value_hex", hex);
        break;
    }
    }
    return item;
}

/**
 * GET_SETTINGS over USB: dialogue on system applet, send flags + target applet id.
 *
 * Neo replies with RESPONSE_GET_SETTINGS whose arg1 is payload byte count; we
 * read_extended() that blob and parse length-prefixed records into JSON items.
 * flags batches settings (NeoTools uses 0, 1, 2, … until empty).
 */
esp_err_t neo_settings_get_json(uint16_t applet_id, uint32_t flags, cJSON *out_array)
{
    if (!out_array) {
        return ESP_ERR_INVALID_ARG;
    }

    neo_device_lock();
    esp_err_t err = neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    const uint32_t args[][3] = {{flags, 1, 4}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_t response;
    neo_message_init(&request, NEO_REQUEST_GET_SETTINGS, args);
    err = neo_device_send_command(&request, NEO_RESPONSE_GET_SETTINGS, NEO_DEFAULT_TIMEOUT_MS, &response);
    if (err != ESP_OK) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return err;
    }

    size_t response_size = neo_message_argument(&response, 1, 4);
    if (response_size == 0 || response_size > 65536) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = malloc(response_size);
    if (!buf) {
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = 0;
    err = neo_device_read_exact(buf, response_size, (int)(response_size * 10 + 600), &read_len);
    if (err == ESP_OK && neo_device_data_checksum(buf, read_len) != neo_message_argument(&response, 5, 2)) {
        err = ESP_ERR_INVALID_CRC;
    }
    neo_device_dialogue_end();
    neo_device_unlock();

    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    size_t offset = 0;
    while (offset + 6 <= read_len) {
        uint16_t length = read_u16_be(buf, offset + 4);
        size_t total = 6 + length + (length & 1U);
        if (offset + total > read_len) {
            break;
        }
        cJSON *item = neo_settings_parse_item(buf + offset, total);
        if (!item) {
            break;
        }
        cJSON_AddItemToArray(out_array, item);
        offset += total;
    }

    free(buf);
    neo_debug_event("GET_SETTINGS applet=0x%04x items=%d", applet_id, cJSON_GetArraySize(out_array));
    return ESP_OK;
}

static void write_u16_be(uint8_t *buf, size_t offset, uint16_t value)
{
    buf[offset] = (uint8_t)(value >> 8);
    buf[offset + 1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *buf, size_t offset, uint32_t value)
{
    buf[offset] = (uint8_t)(value >> 24);
    buf[offset + 1] = (uint8_t)(value >> 16);
    buf[offset + 2] = (uint8_t)(value >> 8);
    buf[offset + 3] = (uint8_t)value;
}

static esp_err_t neo_settings_item_to_raw(cJSON *item, uint8_t **out_buf, size_t *out_len)
{
    if (!item || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *ident_json = cJSON_GetObjectItem(item, "ident");
    cJSON *type_json = cJSON_GetObjectItem(item, "type");
    cJSON *value_json = cJSON_GetObjectItem(item, "value");
    if (!cJSON_IsNumber(ident_json) || !cJSON_IsNumber(type_json)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t type = (uint16_t)type_json->valuedouble;
    uint16_t ident = (uint16_t)ident_json->valuedouble;
    uint8_t payload[64];
    size_t payload_len = 0;

    switch (type) {
    case NEO_SETTINGS_TYPE_RANGE32: {
        if (!cJSON_IsObject(value_json)) {
            return ESP_ERR_INVALID_ARG;
        }
        write_u32_be(payload, 0, (uint32_t)cJSON_GetObjectItem(value_json, "default")->valuedouble);
        write_u32_be(payload, 4, (uint32_t)cJSON_GetObjectItem(value_json, "min")->valuedouble);
        write_u32_be(payload, 8, (uint32_t)cJSON_GetObjectItem(value_json, "max")->valuedouble);
        payload_len = 12;
        break;
    }
    case NEO_SETTINGS_TYPE_OPTION: {
        if (!cJSON_IsArray(value_json)) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, value_json)
        {
            if (!cJSON_IsNumber(entry) || payload_len + 2 > sizeof(payload)) {
                return ESP_ERR_INVALID_ARG;
            }
            write_u16_be(payload, payload_len, (uint16_t)entry->valuedouble);
            payload_len += 2;
        }
        break;
    }
    case NEO_SETTINGS_TYPE_APPLET_ID:
        if (!cJSON_IsNumber(value_json)) {
            return ESP_ERR_INVALID_ARG;
        }
        write_u32_be(payload, 0, (uint32_t)value_json->valuedouble);
        payload_len = 4;
        break;
    case NEO_SETTINGS_TYPE_PASSWORD6:
    case NEO_SETTINGS_TYPE_FILE_PASSWORD:
        if (!cJSON_IsString(value_json)) {
            return ESP_ERR_INVALID_ARG;
        }
        payload_len = strnlen(value_json->valuestring, 6);
        memcpy(payload, value_json->valuestring, payload_len);
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t total = 6 + payload_len + (payload_len & 1U);
    uint8_t *buf = calloc(1, total);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    write_u16_be(buf, 0, type);
    write_u16_be(buf, 2, ident);
    write_u16_be(buf, 4, (uint16_t)payload_len);
    memcpy(buf + 6, payload, payload_len);
    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

esp_err_t neo_settings_set_json(uint16_t applet_id, cJSON *items)
{
    if (!items || !cJSON_IsArray(items)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t total_size = 0;
    uint8_t *blocks[32];
    size_t block_lens[32];
    size_t block_count = 0;
    memset(blocks, 0, sizeof(blocks));

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items)
    {
        if (block_count >= 32) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = neo_settings_item_to_raw(entry, &blocks[block_count], &block_lens[block_count]);
        if (err != ESP_OK) {
            for (size_t i = 0; i < block_count; ++i) {
                free(blocks[i]);
            }
            return err;
        }
        total_size += block_lens[block_count];
        block_count++;
    }

    uint8_t *settings_buf = malloc(total_size);
    if (!settings_buf) {
        for (size_t i = 0; i < block_count; ++i) {
            free(blocks[i]);
        }
        return ESP_ERR_NO_MEM;
    }
    size_t offset = 0;
    for (size_t i = 0; i < block_count; ++i) {
        memcpy(settings_buf + offset, blocks[i], block_lens[i]);
        offset += block_lens[i];
        free(blocks[i]);
    }

    neo_device_lock();
    esp_err_t err = neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
    if (err != ESP_OK) {
        free(settings_buf);
        neo_device_unlock();
        return err;
    }

    uint16_t checksum = neo_device_data_checksum(settings_buf, total_size);
    const uint32_t args[][3] = {{(uint32_t)total_size, 1, 4}, {checksum, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_SET_SETTINGS, args);
    err = neo_device_send_command(&request, NEO_RESPONSE_BLOCK_WRITE, NEO_DEFAULT_TIMEOUT_MS, NULL);
    if (err == ESP_OK) {
        err = neo_device_write(settings_buf, total_size, NEO_DEFAULT_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        neo_message_t response;
        err = neo_device_receive_message(&response, NEO_DEFAULT_TIMEOUT_MS);
        if (err == ESP_OK && neo_message_command(&response) != NEO_RESPONSE_BLOCK_WRITE_DONE) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        const uint32_t set_args[][3] = {{0, 1, 4}, {applet_id, 5, 2}, {0, 0, 0}};
        neo_message_init(&request, NEO_REQUEST_SET_APPLET, set_args);
        err = neo_device_send_command(&request, NEO_RESPONSE_SET_APPLET, NEO_DEFAULT_TIMEOUT_MS, NULL);
    }

    neo_device_dialogue_end();
    neo_device_unlock();
    free(settings_buf);

    if (err == ESP_OK) {
        neo_debug_event("SET_SETTINGS applet=0x%04x bytes=%u", applet_id, (unsigned)total_size);
    }
    return err;
}

static void neo_settings_upsert(cJSON *out_array, const cJSON *item)
{
    const cJSON *ident_json = cJSON_GetObjectItem(item, "ident");
    if (!cJSON_IsNumber(ident_json)) {
        return;
    }
    int ident = ident_json->valueint;
    int count = cJSON_GetArraySize(out_array);
    for (int i = 0; i < count; ++i) {
        cJSON *existing = cJSON_GetArrayItem(out_array, i);
        cJSON *eid = cJSON_GetObjectItem(existing, "ident");
        if (cJSON_IsNumber(eid) && eid->valueint == ident) {
            cJSON_ReplaceItemInArray(out_array, i, cJSON_Duplicate(item, 1));
            return;
        }
    }
    cJSON_AddItemToArray(out_array, cJSON_Duplicate(item, 1));
}

static void neo_settings_merge_batch(cJSON *out_array, cJSON *batch)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, batch) {
        neo_settings_upsert(out_array, item);
    }
}

esp_err_t neo_settings_get_merged_json(uint16_t applet_id, cJSON *out_array)
{
    if (!out_array) {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint32_t flags[] = {0, 7, 15};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        cJSON *batch = cJSON_CreateArray();
        if (!batch) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = neo_settings_get_json(applet_id, flags[i], batch);
        if (err != ESP_OK) {
            cJSON_Delete(batch);
            return err;
        }
        neo_settings_merge_batch(out_array, batch);
        cJSON_Delete(batch);
    }
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        cJSON *batch = cJSON_CreateArray();
        if (!batch) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = neo_settings_get_json(NEO_APPLET_ID_SYSTEM, flags[i], batch);
        if (err != ESP_OK) {
            cJSON_Delete(batch);
            return err;
        }
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, batch) {
            const cJSON *type_json = cJSON_GetObjectItem(item, "type");
            if (!cJSON_IsNumber(type_json)) {
                continue;
            }
            uint16_t type = (uint16_t)type_json->valuedouble;
            if (type == NEO_SETTINGS_TYPE_LABEL || type == NEO_SETTINGS_TYPE_DESCRIPTION) {
                neo_settings_upsert(out_array, item);
            }
        }
        cJSON_Delete(batch);
    }
    return ESP_OK;
}

esp_err_t neo_settings_set_by_ident(uint16_t applet_id, uint16_t ident, cJSON *values)
{
    if (!values || !cJSON_IsArray(values)) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint32_t search_flags[] = {7, 15};
    cJSON *found = NULL;
    for (size_t i = 0; i < sizeof(search_flags) / sizeof(search_flags[0]) && !found; ++i) {
        cJSON *batch = cJSON_CreateArray();
        if (!batch) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = neo_settings_get_json(applet_id, search_flags[i], batch);
        if (err != ESP_OK) {
            cJSON_Delete(batch);
            return err;
        }
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, batch) {
            const cJSON *id_json = cJSON_GetObjectItem(item, "ident");
            if (cJSON_IsNumber(id_json) && (uint16_t)id_json->valuedouble == ident) {
                found = cJSON_Duplicate(item, 1);
                break;
            }
        }
        cJSON_Delete(batch);
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *type_json = cJSON_GetObjectItem(found, "type");
    if (!cJSON_IsNumber(type_json)) {
        cJSON_Delete(found);
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t type = (uint16_t)type_json->valuedouble;
    cJSON *old_value = cJSON_GetObjectItem(found, "value");
    cJSON_DeleteItemFromObject(found, "value");

    switch (type) {
    case NEO_SETTINGS_TYPE_RANGE32: {
        cJSON *range = cJSON_CreateObject();
        if (!range) {
            cJSON_Delete(found);
            return ESP_ERR_NO_MEM;
        }
        cJSON *entry = cJSON_GetArrayItem(values, 0);
        cJSON *min_entry = cJSON_GetArrayItem(values, 1);
        cJSON *max_entry = cJSON_GetArrayItem(values, 2);
        double def_val = cJSON_IsNumber(entry) ? entry->valuedouble : 0;
        cJSON *old_min = cJSON_IsObject(old_value) ? cJSON_GetObjectItem(old_value, "min") : NULL;
        cJSON *old_max = cJSON_IsObject(old_value) ? cJSON_GetObjectItem(old_value, "max") : NULL;
        double min_val = cJSON_IsNumber(min_entry) ? min_entry->valuedouble : (cJSON_IsNumber(old_min) ? old_min->valuedouble : 0);
        double max_val = cJSON_IsNumber(max_entry) ? max_entry->valuedouble : (cJSON_IsNumber(old_max) ? old_max->valuedouble : 0);
        cJSON_AddNumberToObject(range, "default", def_val);
        cJSON_AddNumberToObject(range, "min", min_val);
        cJSON_AddNumberToObject(range, "max", max_val);
        cJSON_AddItemToObject(found, "value", range);
        break;
    }
    case NEO_SETTINGS_TYPE_OPTION: {
        cJSON *options = cJSON_Duplicate(values, 1);
        if (!options) {
            cJSON_Delete(found);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(found, "value", options);
        break;
    }
    case NEO_SETTINGS_TYPE_APPLET_ID: {
        cJSON *entry = cJSON_GetArrayItem(values, 0);
        if (!cJSON_IsNumber(entry)) {
            cJSON_Delete(found);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddNumberToObject(found, "value", entry->valuedouble);
        break;
    }
    case NEO_SETTINGS_TYPE_PASSWORD6:
    case NEO_SETTINGS_TYPE_FILE_PASSWORD: {
        cJSON *entry = cJSON_GetArrayItem(values, 0);
        if (!cJSON_IsString(entry)) {
            cJSON_Delete(found);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddStringToObject(found, "value", entry->valuestring);
        break;
    }
    default:
        cJSON_Delete(found);
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *items = cJSON_CreateArray();
    if (!items) {
        cJSON_Delete(found);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(items, found);
    esp_err_t err = neo_settings_set_json(applet_id, items);
    cJSON_Delete(items);
    return err;
}
