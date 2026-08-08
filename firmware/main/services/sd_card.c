/**
 * @file sd_card.c
 * @brief microSD mount/unmount helpers using ESP-IDF's sdspi driver.
 */

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "device_status.h"
#include "board_config.h"
#include "sd_card.h"

static const char *TAG = "sd_card";
static sdmmc_card_t *mounted_card;
static bool spi_bus_ready;

static void update_capacity_status(void)
{
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    if (esp_vfs_fat_info(SD_CARD_MOUNT_PATH, &total_bytes, &used_bytes) != ESP_OK) {
        device_status_set_sd_card(true, 0, 0);
        return;
    }

    device_status_set_sd_card(true, (size_t)used_bytes, (size_t)total_bytes);
}

static esp_err_t ensure_spi_bus(void)
{
    if (spi_bus_ready) {
        return ESP_OK;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_SD_MOSI_GPIO,
        .miso_io_num = BOARD_SD_MISO_GPIO,
        .sclk_io_num = BOARD_SD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };

    esp_err_t result = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (result == ESP_ERR_INVALID_STATE) {
        spi_bus_ready = true;
        return ESP_OK;
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SD SPI bus unavailable: %s", esp_err_to_name(result));
        return result;
    }

    spi_bus_ready = true;
    return ESP_OK;
}

static esp_err_t mount_card(bool format_if_needed)
{
    if (mounted_card != NULL) {
        return ESP_OK;
    }

    esp_err_t result = ensure_spi_bus();
    if (result != ESP_OK) {
        return result;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SD_CS_GPIO;
    slot_config.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    result = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_PATH, &host, &slot_config,
                                     &mount_config, &mounted_card);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(result));
        device_status_set_sd_card(false, 0, 0);
        return result;
    }

    sdmmc_card_print_info(stdout, mounted_card);
    update_capacity_status();
    ESP_LOGI(TAG, "Mounted microSD at %s", SD_CARD_MOUNT_PATH);
    return ESP_OK;
}

esp_err_t sd_card_mount_if_present(void)
{
    return mount_card(false);
}

esp_err_t sd_card_mount_formatted(void)
{
    sd_card_unmount();
    return mount_card(true);
}

void sd_card_unmount(void)
{
    if (mounted_card == NULL) {
        return;
    }

    esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_PATH, mounted_card);
    mounted_card = NULL;
    device_status_set_sd_card(false, 0, 0);
}

bool sd_card_is_mounted(void)
{
    return mounted_card != NULL;
}
