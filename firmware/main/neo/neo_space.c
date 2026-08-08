/**
 * @file neo_space.c
 * @brief Query free ROM/RAM and per-applet usage (NeoTools get_available_space parity).
 *
 * HOW SPACE QUERIES WORK
 * ----------------------
 * Both commands run on the **system applet** (id 0x0000), not on AlphaWord.
 * NeoTools opens a dialogue (hello → reset → switch to 0x0000), sends one framed
 * 8-byte packet, waits for the matching response, then closes the dialogue.
 *
 * GET_AVAIL_SPACE (request 0x1a → response 0x58)
 *   Request has no arguments.
 *   Response fields (big-endian in the 8-byte packet):
 *     bytes 1-4  → free_rom (bytes of flash still available for applets)
 *     bytes 5-6  → free_ram units; multiply by 256 to get bytes (NeoTools does this)
 *
 * GET_USED_SPACE (request 0x1b → response 0x59)
 *   Request arguments:
 *     bytes 1-4  → select32 = 1 (largest file mode; 0 would mean all files)
 *     bytes 5-6  → applet_id (e.g. 0xA000 for AlphaWord)
 *   Response fields:
 *     bytes 1-4  → ram_used (bytes of RAM used by that applet)
 *     bytes 5-6  → file_count (number of files in the applet)
 *
 * neo_applet_install and neo_file_create call these before writing so we fail
 * early with ESP_ERR_NO_MEM instead of halfway through a block transfer.
 */

#include "neo_space.h"

#include "neo_applet.h"
#include "neo_debug.h"
#include "neo_device.h"
#include "neo_message.h"

/** Open dialogue on system applet — same entry point NeoTools uses for space/info. */
static esp_err_t neo_space_dialogue_start(void)
{
    return neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
}

esp_err_t neo_space_get_available(neo_avail_space_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    neo_device_lock();
    esp_err_t err = neo_space_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    /* Empty-args GET_AVAIL_SPACE; Neo replies with RESPONSE_GET_AVAIL_SPACE. */
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_GET_AVAIL_SPACE, NULL);

    neo_message_t response;
    err = neo_device_send_command(&request, NEO_RESPONSE_GET_AVAIL_SPACE, NEO_DEFAULT_TIMEOUT_MS, &response);
    if (err == ESP_OK) {
        out->free_rom = neo_message_argument(&response, 1, 4);
        /* Neo reports RAM in 256-byte blocks; NeoTools multiplies by 256. */
        out->free_ram = neo_message_argument(&response, 5, 2) * 256U;
        neo_debug_event("avail_space rom=%lu ram=%lu", (unsigned long)out->free_rom,
                        (unsigned long)out->free_ram);
    }

    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}

esp_err_t neo_space_get_used(uint16_t applet_id, neo_used_space_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    neo_device_lock();
    esp_err_t err = neo_space_dialogue_start();
    if (err != ESP_OK) {
        neo_device_unlock();
        return err;
    }

    /* select32=1 requests usage for the applet's largest file slot set (NeoTools default). */
    const uint32_t args[][3] = {{0x00000001UL, 1, 4}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_GET_USED_SPACE, args);

    neo_message_t response;
    err = neo_device_send_command(&request, NEO_RESPONSE_GET_USED_SPACE, NEO_DEFAULT_TIMEOUT_MS, &response);
    if (err == ESP_OK) {
        out->ram_used = neo_message_argument(&response, 1, 4);
        out->file_count = (uint16_t)neo_message_argument(&response, 5, 2);
        neo_debug_event("used_space applet=0x%04x ram=%lu files=%u", applet_id, (unsigned long)out->ram_used,
                        (unsigned)out->file_count);
    }

    neo_device_dialogue_end();
    neo_device_unlock();
    return err;
}
