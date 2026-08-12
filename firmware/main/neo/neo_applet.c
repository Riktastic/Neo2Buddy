/**
 * @file neo_applet.c
 * @brief List, install, remove, and fetch SmartApplets over the system applet.
 *
 * Every operation follows the same pattern NeoTools uses:
 *   neo_device_lock()
 *   neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM)  // 0x0000
 *   send one or more 8-byte commands + optional read_extended/write_applet_content
 *   neo_device_dialogue_end()
 *   neo_device_unlock()
 *
 * LIST_APPLETS batches up to 7 headers (0x84 bytes each) per request.
 * WRITE_APPLET streams the full .os3kapp bytes with PROGRAMMING_APPLET_BLOCK
 * acknowledgements between 1 KiB chunks (see neo_device_write_applet_content).
 */

#include "neo_applet.h"

#include <stdlib.h>
#include <string.h>

#include "neo_device.h"
#include "neo_debug.h"
#include "neo_message.h"
#include "neo_space.h"
#include "esp_log.h"

static const char *TAG = "neo_applet";

#define NEO_APPLET_SIGNATURE_START 0xc0ffeeadUL
#define NEO_APPLET_SIGNATURE_END 0xcafefeedUL
#define NEO_SYSTEM_APPLET_ID 0x0000

/* Simple big-endian readers used to parse headers in the applet blob. */
static uint32_t read_u32(const uint8_t *buffer, size_t offset)
{
    return ((uint32_t)buffer[offset] << 24) | ((uint32_t)buffer[offset + 1] << 16) |
           ((uint32_t)buffer[offset + 2] << 8) | buffer[offset + 3];
}

static uint16_t read_u16(const uint8_t *buffer, size_t offset)
{
    return ((uint16_t)buffer[offset] << 8) | buffer[offset + 1];
}

static void read_string(char *destination, size_t destination_size, const uint8_t *source, size_t source_size)
{
    size_t length = 0;
    while (length < source_size && source[length] != '\0') length++;
    if (length >= destination_size) length = destination_size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

/* Validate and extract the header fields into `info`. */
static esp_err_t parse_header(const uint8_t *content, size_t content_length, neo_applet_info_t *info)
{
    if (!content || !info || content_length < NEO_APPLET_HEADER_SIZE) {
        neo_debug_event("applet header parse: invalid size %u", (unsigned)content_length);
        return ESP_ERR_INVALID_SIZE;
    }
    if (read_u32(content, 0) != NEO_APPLET_SIGNATURE_START) {
        neo_debug_event("applet header parse: bad signature 0x%08lx", (unsigned long)read_u32(content, 0));
        return ESP_ERR_INVALID_CRC;
    }
    memset(info, 0, sizeof(*info));
    info->rom_size = read_u32(content, 0x04);
    info->ram_size = read_u32(content, 0x08);
    info->settings_offset = read_u32(content, 0x0c);
    info->flags = read_u32(content, 0x10);
    info->applet_id = read_u16(content, 0x14);
    info->file_count = content[0x17];
    read_string(info->name, sizeof(info->name), content + 0x18, 36);
    info->version_major = content[0x3c];
    info->version_minor = content[0x3d];
    info->version_revision = content[0x3e];
    info->file_space = read_u32(content, 0x80);
    return ESP_OK;
}

esp_err_t neo_applet_inspect(const uint8_t *content, size_t content_length, neo_applet_info_t *info)
{
    esp_err_t result = parse_header(content, content_length, info);
    if (result != ESP_OK) {
        neo_debug_event("applet inspect header failed: %s", esp_err_to_name(result));
        return result;
    }
    if (read_u32(content, content_length - 4) != NEO_APPLET_SIGNATURE_END) {
        neo_debug_event("applet inspect bad footer id=0x%04x", info->applet_id);
        return ESP_ERR_INVALID_CRC;
    }
    neo_debug_event("applet inspect ok id=0x%04x name=%s rom=%lu", info->applet_id, info->name,
                    (unsigned long)info->rom_size);
    return ESP_OK;
}

/* Query the device for installed applets in batches and fill `applets`. */
esp_err_t neo_applet_list(neo_applet_info_t *applets, size_t capacity, size_t *out_count)
{
    if (!applets || !out_count) return ESP_ERR_INVALID_ARG;
    neo_device_lock();
    *out_count = 0;
    neo_debug_event("LIST_APPLETS start");
    esp_err_t result = neo_device_dialogue_start(NEO_SYSTEM_APPLET_ID);
    if (result != ESP_OK) {
        neo_debug_event("LIST_APPLETS dialogue_start failed: %s", esp_err_to_name(result));
        neo_device_unlock();
        return result;
    }

    while (*out_count < capacity) {
        const uint32_t args[][3] = {{*out_count, 1, 4}, {NEO_APPLET_LIST_BATCH_SIZE, 5, 2}, {0, 0, 0}};
        neo_message_t request;
        neo_message_t response;
        neo_message_init(&request, NEO_REQUEST_LIST_APPLETS, args);
        result = neo_device_send_command(&request, NEO_RESPONSE_LIST_APPLETS, 1000, &response);
        if (result != ESP_OK) {
            neo_debug_event("LIST_APPLETS batch at %u failed: %s", (unsigned)*out_count,
                            esp_err_to_name(result));
            break;
        }
        size_t byte_count = neo_message_argument(&response, 1, 4);
        if (byte_count == 0 || byte_count % NEO_APPLET_HEADER_SIZE != 0 ||
            byte_count > NEO_APPLET_LIST_BATCH_SIZE * NEO_APPLET_HEADER_SIZE) {
            if (byte_count != 0) {
                neo_debug_event("LIST_APPLETS bad byte_count %u at offset %u", (unsigned)byte_count,
                                (unsigned)*out_count);
                result = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        uint8_t *raw = malloc(NEO_APPLET_LIST_BATCH_SIZE * NEO_APPLET_HEADER_SIZE);
        if (!raw) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        size_t read_length = 0;
        result = neo_device_read_exact(raw, byte_count, (int)(byte_count * 10 + 600), &read_length);
        if (result != ESP_OK || read_length != byte_count) {
            neo_debug_event("LIST_APPLETS payload read failed: %s (got %u/%u)", esp_err_to_name(result),
                            (unsigned)read_length, (unsigned)byte_count);
            free(raw);
            result = result != ESP_OK ? result : ESP_ERR_TIMEOUT;
            break;
        }
        if (neo_device_data_checksum(raw, read_length) != neo_message_argument(&response, 5, 2)) {
            neo_debug_event("LIST_APPLETS CRC mismatch batch at %u", (unsigned)*out_count);
            free(raw);
            result = ESP_ERR_INVALID_CRC;
            break;
        }
        size_t count = byte_count / NEO_APPLET_HEADER_SIZE;
        if (count > capacity - *out_count) {
            neo_debug_event("LIST_APPLETS capacity exceeded need %u have %u", (unsigned)(*out_count + count),
                            (unsigned)capacity);
            free(raw);
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        for (size_t index = 0; index < count; index++) {
            result = parse_header(raw + index * NEO_APPLET_HEADER_SIZE, NEO_APPLET_HEADER_SIZE,
                                  &applets[*out_count + index]);
            if (result != ESP_OK) {
                neo_debug_event("LIST_APPLETS parse header failed at slot %u", (unsigned)(*out_count + index));
                break;
            }
        }
        free(raw);
        if (result != ESP_OK) break;
        *out_count += count;
        if (count < NEO_APPLET_LIST_BATCH_SIZE) break;
    }
    esp_err_t end_result = neo_device_dialogue_end();
    esp_err_t final = result == ESP_OK ? end_result : result;
    if (final == ESP_OK) {
        neo_debug_event("LIST_APPLETS ok count=%u", (unsigned)*out_count);
        ESP_LOGI(TAG, "listed %u applets", (unsigned)*out_count);
    } else {
        neo_debug_event("LIST_APPLETS failed: %s (count=%u)", esp_err_to_name(final), (unsigned)*out_count);
        ESP_LOGW(TAG, "list failed: %s", esp_err_to_name(final));
    }
    neo_device_unlock();
    return final;
}

/* Remove an applet by requesting the system applet to delete it. */
esp_err_t neo_applet_remove(uint16_t applet_id)
{
    neo_device_lock();
    neo_debug_event("REMOVE_APPLET start id=0x%04x", applet_id);
    const uint32_t args[][3] = {{5, 1, 4}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_REMOVE_APPLET, args);
    esp_err_t result = neo_device_dialogue_start(NEO_SYSTEM_APPLET_ID);
    if (result == ESP_OK) {
        result = neo_device_send_command(&request, NEO_RESPONSE_REMOVE_APPLET, NEO_DEFAULT_TIMEOUT_MS, NULL);
    }
    (void)neo_device_dialogue_end();
    if (result == ESP_OK) {
        neo_debug_event("REMOVE_APPLET ok id=0x%04x", applet_id);
    } else {
        neo_debug_event("REMOVE_APPLET failed id=0x%04x: %s", applet_id, esp_err_to_name(result));
        ESP_LOGW(TAG, "remove applet 0x%04x failed: %s", applet_id, esp_err_to_name(result));
    }
    neo_device_unlock();
    return result;
}

/* Erase all applets. */
esp_err_t neo_applet_remove_all(void)
{
    neo_device_lock();
    neo_debug_event("ERASE_APPLETS start");
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_ERASE_APPLETS, NULL);
    esp_err_t result = neo_device_dialogue_start(NEO_SYSTEM_APPLET_ID);
    if (result == ESP_OK) {
        result = neo_device_send_command(&request, NEO_RESPONSE_ERASE_APPLETS, 90000, NULL);
    }
    (void)neo_device_dialogue_end();
    if (result == ESP_OK) {
        neo_debug_event("ERASE_APPLETS ok");
    } else {
        neo_debug_event("ERASE_APPLETS failed: %s", esp_err_to_name(result));
        ESP_LOGW(TAG, "erase applets failed: %s", esp_err_to_name(result));
    }
    neo_device_unlock();
    return result;
}

/* Fetch a full applet blob into `content` using the BLOCK_READ flow (NeoTools
 * applets fetch → REQUEST_READ_APPLET 0x0f on system dialogue 0x0000). */
esp_err_t neo_applet_fetch(uint16_t applet_id, uint8_t *content, size_t capacity, size_t *out_length)
{
    if (!content || !out_length) return ESP_ERR_INVALID_ARG;
    neo_device_lock();
    neo_debug_event("READ_APPLET start id=0x%04x cap=%u", applet_id, (unsigned)capacity);
    const uint32_t args[][3] = {{0, 1, 4}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_t response;
    neo_message_init(&request, NEO_REQUEST_READ_APPLET, args);
    esp_err_t result = neo_device_dialogue_start(NEO_SYSTEM_APPLET_ID);
    size_t expected = 0;
    if (result == ESP_OK) {
        result = neo_device_send_command(&request, NEO_RESPONSE_READ_FILE, 1000, &response);
    }
    if (result == ESP_OK) {
        expected = neo_message_argument(&response, 1, 4);
        neo_debug_event("READ_APPLET id=0x%04x expect=%u bytes", applet_id, (unsigned)expected);
        if (expected == 0) {
            result = ESP_ERR_NOT_FOUND;
        } else if (expected > capacity) {
            neo_debug_event("READ_APPLET too large expect=%u cap=%u", (unsigned)expected, (unsigned)capacity);
            result = ESP_ERR_NO_MEM;
        } else {
            result = neo_device_read_extended(content, capacity, expected, out_length);
        }
    }
    esp_err_t end_result = neo_device_dialogue_end();
    esp_err_t final = result == ESP_OK ? end_result : result;
    if (final == ESP_OK && *out_length != expected) {
        neo_debug_event("READ_APPLET length mismatch got=%u expect=%u", (unsigned)*out_length,
                        (unsigned)expected);
        final = ESP_ERR_INVALID_SIZE;
    }
    /* Regular SmartApplets are C0FFEEAD…CAFEFEED packages. Applet id 0 is a raw
     * firmware ROM dump (NeoTools: applets fetch 0) — skip package validation. */
    if (final == ESP_OK && applet_id != 0) {
        neo_applet_info_t info;
        final = neo_applet_inspect(content, *out_length, &info);
        if (final == ESP_OK && info.applet_id != applet_id) {
            neo_debug_event("READ_APPLET id mismatch header=0x%04x asked=0x%04x", info.applet_id, applet_id);
            final = ESP_ERR_INVALID_STATE;
        }
    }
    if (final == ESP_OK) {
        neo_debug_event("READ_APPLET ok id=0x%04x bytes=%u", applet_id, (unsigned)*out_length);
    } else {
        neo_debug_event("READ_APPLET failed id=0x%04x: %s", applet_id, esp_err_to_name(final));
    }
    neo_device_unlock();
    return final;
}

/* Install an applet: inspect it, optionally remove existing with same ID, then
 * perform WRITE_APPLET followed by the block write and finalize steps. */
esp_err_t neo_applet_install(const uint8_t *content, size_t content_length, bool replace_existing)
{
    neo_applet_info_t applet;
    neo_debug_event("WRITE_APPLET start len=%u replace=%d", (unsigned)content_length, replace_existing ? 1 : 0);
    esp_err_t result = neo_applet_inspect(content, content_length, &applet);
    if (result != ESP_OK) return result;
    if (applet.rom_size > 0xff000000UL || applet.ram_size + applet.file_space > 0xff000000UL) {
        neo_debug_event("WRITE_APPLET invalid sizes rom=%lu ram=%lu space=%lu", (unsigned long)applet.rom_size,
                        (unsigned long)applet.ram_size, (unsigned long)applet.file_space);
        return ESP_ERR_INVALID_SIZE;
    }

    neo_device_lock();
    neo_applet_info_t *installed = calloc(32, sizeof(*installed));
    if (!installed) {
        neo_device_unlock();
        return ESP_ERR_NO_MEM;
    }
    size_t installed_count = 0;
    result = neo_applet_list(installed, 32, &installed_count);
    if (result != ESP_OK) {
        neo_debug_event("WRITE_APPLET list existing failed: %s", esp_err_to_name(result));
        free(installed);
        neo_device_unlock();
        return result;
    }
    for (size_t index = 0; index < installed_count; index++) {
        if (installed[index].applet_id != applet.applet_id) continue;
        if (!replace_existing) {
            neo_debug_event("WRITE_APPLET duplicate id=0x%04x", applet.applet_id);
            free(installed);
            neo_device_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        result = neo_applet_remove(applet.applet_id);
        if (result != ESP_OK) {
            neo_debug_event("WRITE_APPLET remove existing failed: %s", esp_err_to_name(result));
            free(installed);
            neo_device_unlock();
            return result;
        }
        break;
    }
    free(installed);

    uint32_t required_size = applet.ram_size + applet.file_space;
    neo_avail_space_t space = {0};
    result = neo_space_get_available(&space);
    if (result != ESP_OK) {
        /* Fail closed: never write an applet when free space is unknown. */
        neo_debug_event("WRITE_APPLET space query failed: %s", esp_err_to_name(result));
        neo_device_unlock();
        return result;
    }
    if (applet.rom_size > space.free_rom || required_size > space.free_ram) {
        neo_debug_event("WRITE_APPLET insufficient space need_rom=%lu free_rom=%lu need_ram=%lu free_ram=%lu",
                        (unsigned long)applet.rom_size, (unsigned long)space.free_rom,
                        (unsigned long)required_size, (unsigned long)space.free_ram);
        neo_device_unlock();
        return ESP_ERR_NO_MEM;
    }
    uint32_t packed_size = applet.rom_size | ((required_size & 0xffff0000UL) << 8);
    const uint32_t args[][3] = {{packed_size, 1, 4}, {required_size, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_WRITE_APPLET, args);
    result = neo_device_dialogue_start(NEO_SYSTEM_APPLET_ID);
    if (result == ESP_OK) {
        result = neo_device_send_command(&request, NEO_RESPONSE_WRITE_APPLET, 5000, NULL);
    }
    if (result == ESP_OK) {
        result = neo_device_write_applet_content(content, content_length);
    }
    if (result == ESP_OK) {
        /* NeoTools: write FINALIZE (long USB timeout), then retry receive until RESPONSE_FINALIZE. */
        neo_message_init(&request, NEO_REQUEST_FINALIZE_WRITING_APPLET, NULL);
        result = neo_device_write(request.data, sizeof(request.data), 24000);
        if (result == ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            for (int retry = 0; retry < 10; retry++) {
                neo_message_t response;
                esp_err_t rx = neo_device_receive_message(&response, 5000);
                if (rx != ESP_OK) {
                    neo_debug_event("FINALIZE waiting retry=%d: %s", retry, esp_err_to_name(rx));
                    continue;
                }
                uint8_t cmd = neo_message_command(&response);
                if (cmd == NEO_RESPONSE_FINALIZE_WRITING_APPLET) {
                    result = ESP_OK;
                    break;
                }
                if (cmd == NEO_ERROR_OUTOFMEMORY) {
                    neo_debug_event("FINALIZE Neo out of memory");
                    result = ESP_ERR_NO_MEM;
                    break;
                }
                if (cmd >= 0x86) {
                    neo_debug_event("FINALIZE Neo error 0x%02x: %s", cmd, neo_message_error_string(cmd));
                    result = ESP_FAIL;
                    break;
                }
                neo_debug_event("FINALIZE unexpected response 0x%02x retry=%d", cmd, retry);
            }
        }
    }
    (void)neo_device_dialogue_end();
    if (result == ESP_OK) {
        neo_debug_event("WRITE_APPLET ok id=0x%04x name=%s", applet.applet_id, applet.name);
        ESP_LOGI(TAG, "installed applet 0x%04x (%s)", applet.applet_id, applet.name);
    } else {
        neo_debug_event("WRITE_APPLET failed id=0x%04x: %s", applet.applet_id, esp_err_to_name(result));
        ESP_LOGW(TAG, "install failed: %s", esp_err_to_name(result));
    }
    neo_device_unlock();
    return result;
}
