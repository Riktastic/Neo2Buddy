/**
 * @file neo_space.h
 * @brief Query free ROM/RAM and per-applet usage before install/write.
 *
 * Neo exposes capacity through framed commands on the system applet:
 *   GET_AVAIL_SPACE (0x1a) → free_rom (u32), free_ram (u16 * 256)
 *   GET_USED_SPACE (0x1b)  → ram_used, file_count for one applet id
 *
 * neo_applet_install and neo_file_create check these before writing so we fail
 * early with OUTOFMEMORY instead of mid-transfer on the device.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t free_rom;
    uint32_t free_ram;
} neo_avail_space_t;

typedef struct {
    uint32_t ram_used;
    uint16_t file_count;
} neo_used_space_t;

esp_err_t neo_space_get_available(neo_avail_space_t *out);
esp_err_t neo_space_get_used(uint16_t applet_id, neo_used_space_t *out);
