/**
 * @file sd_card.h
 * @brief microSD mount helpers (SPI, FAT, /sdcard VFS).
 *
 * When HAVE_SDCARD is set, neo_import and file_manager prefer /sdcard/neo for
 * backups. Mount is lazy (sd_card_mount_if_present) so boot works without a
 * card inserted. Unmount before format; sd_format.c handles destructive ops.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_PATH "/sdcard"

bool sd_card_is_mounted(void);
esp_err_t sd_card_mount_if_present(void);
esp_err_t sd_card_mount_formatted(void);
void sd_card_unmount(void);
