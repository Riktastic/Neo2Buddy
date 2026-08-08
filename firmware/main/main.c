#/**************************************************************************
 * @file main.c
 * @brief Application entry for Neo2 Buddy firmware.
 ***************************************************************************/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "board_config.h"
#include "device_status.h"
#include "sd_card.h"
#include "settings.h"
#include "usb_host_neo.h"
#include "neo_autobackup.h"
#include "neo_debug.h"
#include "neo_live.h"
#include "neo_conv.h"
#include "cloud_sync.h"
#include "esp_spiffs.h"
#include "web_api_http.h"
#include "esp_http_server.h"
#include <stdio.h>
#include <sys/stat.h>
#include "auth.h"
#include "wifi_manager.h"
#include "log_buffer.h"
#include "health_check.h"
#include "self_test.h"
#include "ble_hid.h"
#include "battery_monitor.h"
#include "display.h"

#ifdef CONFIG_BUDDY_UART_CMD
#include "uart_cmd.h"
#endif

static const char *TAG = "neo2_buddy";

/** Printed once services are up so it lands after the ESP-IDF boot log noise. */
static void print_boot_welcome(void)
{
    printf("\n");
    printf("  ============================================================\n");
    printf("   AlphaSmart Neo2 Buddy\n");
    printf("   ESP32-S3 companion for the AlphaSmart NEO2\n");
    printf("\n");
    printf("   Project : https://github.com/Riktastic/Neo2Buddy\n");
    printf("   Author  : https://github.com/Riktastic\n");
    printf("\n");
    printf("   Open the web portal over Wi-Fi, or type 'help' on this\n");
    printf("   serial console (login with your portal password).\n");
    printf("  ============================================================\n");
    printf("\n");
    fflush(stdout);
}

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
    }
}

static void initialize_persistent_storage(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* httpd reserves 3 sockets internally; keep this low so mDNS/SNTP/Wi‑Fi still fit
     * under CONFIG_LWIP_MAX_SOCKETS (see sdkconfig.defaults). */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 12288;
    config.recv_wait_timeout = 120;
    config.send_wait_timeout = 120;
    httpd_handle_t server = NULL;

    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Starting HTTP server (free heap=%u min=%u, max_open_sockets=%u)",
             (unsigned)free_heap, (unsigned)min_heap, (unsigned)config.max_open_sockets);

    esp_err_t err = httpd_start(&server, &config);
    if (err == ESP_OK) {
        esp_err_t reg = web_api_register(server);
        if (reg != ESP_OK) {
            ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(reg));
        } else {
            web_api_note_boot_time();
        }
        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s (free heap=%u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
    }
}

/** Heavy init runs on a dedicated task — Wi-Fi first to limit peak USB current draw. */
static void startup_task(void *arg)
{
    (void)arg;

    wifi_manager_init();

    device_settings_t net_settings;
    settings_load(&net_settings);
    const bool home_mode = net_settings.network_mode == SETTINGS_NETWORK_HOME &&
                           net_settings.wifi_ssid[0] != '\0';

    if (home_mode) {
        ESP_LOGI(TAG, "Waiting for home Wi-Fi or recovery hotspot (up to 50s)...");
        for (int i = 0; i < 50 && !wifi_manager_is_connected() && !wifi_manager_is_recovery_mode(); i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "Home Wi-Fi up");
        } else if (!wifi_manager_is_recovery_mode()) {
            ESP_LOGW(TAG, "Home Wi-Fi not connected — forcing recovery hotspot");
            wifi_manager_force_recovery_ap();
        } else {
            ESP_LOGW(TAG, "Recovery hotspot active");
        }
    }

    /* Portal first — BLE/USB already consume a lot of internal RAM/sockets. */
    health_check_init();
    start_http_server();

#if HAVE_OLED
    display_init();
#endif

    if (home_mode && !wifi_manager_is_connected() && !wifi_manager_is_recovery_mode()) {
        ESP_LOGW(TAG, "Skipping BLE until network is reachable");
    } else {
        ble_hid_init();
    }

#if HAVE_SDCARD
    esp_err_t sd_res = sd_card_mount_if_present();
    if (sd_res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(sd_res));
    }
#endif

    /* Let late service logs flush, then print the welcome banner last. */
    vTaskDelay(pdMS_TO_TICKS(200));
    print_boot_welcome();

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();

    initialize_persistent_storage();
    device_status_init();
    neo_live_init();
    log_buffer_init();

    device_settings_t settings;
    ESP_ERROR_CHECK(settings_load(&settings));
    ESP_LOGW(TAG, "Neo2 Buddy starting (reset reason=%d %s)",
             (int)reset_reason, reset_reason_str(reset_reason));

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiflash",
        .partition_label = "littlefs",
        .max_files = 16,
        .format_if_mount_failed = false
    };
    esp_err_t spiffs_res = esp_vfs_spiffs_register(&conf);
    if (spiffs_res == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiflash");
        struct stat portal_index;
        if (stat("/spiflash/index.html", &portal_index) != 0) {
            ESP_LOGE(TAG, "Portal index.html missing from SPIFFS - web UI will not load");
        }
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(spiffs_res));
    }

    usb_host_neo_init();
    neo_debug_init();
    neo_autobackup_init();
    cloud_sync_init();
    if (neo_charmap_init() != ESP_OK) {
        ESP_LOGW(TAG, "Neo character maps failed to load; text import/export may be limited");
    }
    auth_init();

#if CONFIG_BUDDY_UART_CMD
    if (uart_cmd_init() != ESP_OK) {
        ESP_LOGW(TAG, "UART command console not started");
    }
#endif

#if CONFIG_BUDDY_RUN_SELF_TEST
    self_test_run();
#endif

#if HAVE_BATTERY
    battery_monitor_init();
#endif

    if (xTaskCreate(startup_task, "startup", 12288, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create startup task");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
