/**
 * @file neo_file_extra.c
 * @brief Resolve Neo files by human name or "File N" space number.
 *
 * NeoTools lets users target documents by display name or by file-space key
 * (the F1..F8 shortcuts). We scan indices 1..until NOT_FOUND and match:
 *   - "3" or space_number 3 → third file space
 *   - exact name string → attrs.name
 *
 * write_or_create: update existing slot or neo_file_create() a new one.
 */

#include "neo_file.h"

#include <stdlib.h>
#include <string.h>

#include "neo_device.h"
#include "neo_applet.h"

esp_err_t neo_file_find_by_name_or_space(uint16_t applet_id, const char *name_or_space, neo_file_attr_t *out)
{
    if (!name_or_space || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_file_attr_t *listed = calloc(32, sizeof(*listed));
    if (!listed) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;

    neo_device_lock();
    esp_err_t err = neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
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
        /* Parse into listed[count] — duplicate of neo_file_attr_from_raw fields. */
        memset(&listed[count], 0, sizeof(listed[count]));
        listed[count].file_index = index;
        memcpy(listed[count].name, attr_buf, 15);
        listed[count].name[15] = '\0';
        memcpy(listed[count].password, attr_buf + 0x10, 7);
        listed[count].password[7] = '\0';
        listed[count].min_size = ((uint32_t)attr_buf[0x18] << 24) | ((uint32_t)attr_buf[0x19] << 16) |
                                 ((uint32_t)attr_buf[0x1a] << 8) | attr_buf[0x1b];
        listed[count].alloc_size = ((uint32_t)attr_buf[0x1c] << 24) | ((uint32_t)attr_buf[0x1d] << 16) |
                                   ((uint32_t)attr_buf[0x1e] << 8) | attr_buf[0x1f];
        listed[count].flags = ((uint32_t)attr_buf[0x20] << 24) | ((uint32_t)attr_buf[0x21] << 16) |
                              ((uint32_t)attr_buf[0x22] << 8) | attr_buf[0x23];
        listed[count].file_space = attr_buf[0x25];
        listed[count].space_number = neo_file_space_to_number(listed[count].file_space);
        count++;
    }
    neo_device_dialogue_end();
    neo_device_unlock();

    int want_space = 0;
    if (name_or_space[0] >= '1' && name_or_space[0] <= '8' && name_or_space[1] == '\0') {
        want_space = name_or_space[0] - '0';
    }
    for (size_t i = 0; i < count; i++) {
        if (want_space >= 1 && want_space <= 8) {
            if (listed[i].space_number == want_space) {
                *out = listed[i];
                free(listed);
                return ESP_OK;
            }
        } else if (strcmp(listed[i].name, name_or_space) == 0) {
            *out = listed[i];
            free(listed);
            return ESP_OK;
        }
    }
    free(listed);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t neo_file_clear_by_name_or_space(uint16_t applet_id, const char *name_or_space)
{
    neo_file_attr_t attrs;
    esp_err_t err = neo_file_find_by_name_or_space(applet_id, name_or_space, &attrs);
    if (err != ESP_OK) {
        return err;
    }
    return neo_file_clear(applet_id, attrs.file_index);
}

esp_err_t neo_file_write_or_create(uint16_t applet_id, const char *name_or_space, const char *password,
                                   const uint8_t *data, size_t length)
{
    if (!name_or_space || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_file_attr_t attrs;
    esp_err_t err = neo_file_find_by_name_or_space(applet_id, name_or_space, &attrs);
    if (err == ESP_OK) {
        return neo_file_write_raw(applet_id, attrs.file_index, data, length);
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    const char *pw = (password && password[0]) ? password : "write";
    return neo_file_create(applet_id, name_or_space, pw, data, length);
}
