#/**************************************************************************
 * @file main.c
 * @brief Application entry for Neo2 Buddy firmware.
 ***************************************************************************/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
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

#if HAVE_WIFI_WEB
#include "web_api_http.h"
#include "esp_http_server.h"
#endif

#ifdef CONFIG_BUDDY_NEO_LINK
#include "neo_link_applet.h"
#endif

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

#if HAVE_WIFI_WEB
    char ip[16] = {0};
    const bool have_ip = wifi_manager_get_ip(ip, sizeof(ip));
    device_settings_t net;
    settings_load(&net);
    const bool recovery = wifi_manager_is_recovery_mode();
    const bool home_up = wifi_manager_is_connected();
    const bool ap_mode = recovery ||
                         net.network_mode == SETTINGS_NETWORK_DIRECT ||
                         (!home_up && have_ip);

    if (ap_mode) {
        const char *ssid = net.hotspot_ssid[0] ? net.hotspot_ssid : "Neo2-Buddy";
        const char *pw = net.hotspot_password[0] ? net.hotspot_password : "neo2buddy";
        printf("   Network : SoftAP / Direct access%s\n", recovery ? " (recovery)" : "");
        printf("   SSID    : %s\n", ssid);
        printf("   Wi-Fi PW: %s\n", pw);
        printf("   Portal  : http://192.168.4.1/\n");
        if (recovery || net.network_mode == SETTINGS_NETWORK_DIRECT) {
            printf("   Setup   : http://192.168.4.1/setup.html\n");
        }
        printf("\n");
        printf("   Connect your phone/laptop to the SSID above, then open the portal URL.\n");
    } else if (home_up && have_ip) {
        printf("   Network : Home Wi-Fi\n");
        if (net.wifi_ssid[0]) {
            printf("   Joined  : %s\n", net.wifi_ssid);
        }
        printf("   Portal  : http://%s/\n", ip);
        printf("\n");
        printf("   Open the portal URL on your LAN (same Wi-Fi as the buddy).\n");
    } else {
        printf("   Network : Wi-Fi still starting (check logs for SoftAP / IP)\n");
        printf("   Tip     : SoftAP portal is usually http://192.168.4.1/\n");
    }
#else
    printf("   Profile : UART-only (Wi-Fi / web portal disabled at build time)\n");
    printf("   Neo USB : plug Neo into OTG for backups and SmartApplets\n");
#endif

#if CONFIG_BUDDY_UART_CMD
    printf("   Console : type 'help' (login with your portal password)\n");
#else
    printf("   Console : UART command console disabled in this build\n");
#endif
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

#if HAVE_WIFI_WEB
static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 80;
    config.uri_match_fn = web_api_uri_match;
    /* httpd reserves ~3 sockets internally. Browsers open several parallel TCP
     * connections for CSS/JS; 4 slots caused SoftAP asset timeouts
     * (ERR_CONNECTION_TIMED_OUT). Keep headroom under CONFIG_LWIP_MAX_SOCKETS=16. */
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    /* Short waits so dead SoftAP clients do not pin slots for minutes. */
    config.recv_wait_timeout = 8;
    config.send_wait_timeout = 8;
    /* Keep the httpd task stack in INTERNAL DRAM. Handlers read SPIFFS/flash;
     * a PSRAM stack panics when the cache is disabled for flash ops. */
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    httpd_handle_t server = NULL;

    const size_t stack_candidates[] = { 12288, 10240, 8192, 6144 };
    size_t free_heap = esp_get_free_heap_size();
    size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "Starting HTTP server (free=%u largest_internal=%u free_spiram=%u, "
             "max_open_sockets=%u, stack→INTERNAL)",
             (unsigned)free_heap, (unsigned)largest_internal, (unsigned)free_spiram,
             (unsigned)config.max_open_sockets);

    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(stack_candidates) / sizeof(stack_candidates[0]); ++i) {
        const size_t stack = stack_candidates[i];
        if (stack + 2048 > largest_internal) {
            ESP_LOGW(TAG, "skip httpd stack=%u (largest_internal=%u)",
                     (unsigned)stack, (unsigned)largest_internal);
            continue;
        }
        config.stack_size = stack;
        err = httpd_start(&server, &config);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "httpd task stack=%u (INTERNAL)", (unsigned)stack);
            break;
        }
        ESP_LOGW(TAG, "httpd_start stack=%u failed: %s — trying smaller",
                 (unsigned)stack, esp_err_to_name(err));
    }

    if (err == ESP_OK) {
        esp_err_t reg = web_api_register(server);
        if (reg != ESP_OK) {
            ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(reg));
        } else {
            web_api_note_boot_time();
        }
        size_t after =
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "HTTP server started (largest_internal now=%u)", (unsigned)after);
    } else {
        ESP_LOGE(TAG,
                 "Failed to start HTTP server: %s (free=%u largest_internal=%u free_spiram=%u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    }
}
#endif /* HAVE_WIFI_WEB */

/** Heavy init runs on a dedicated task — Wi-Fi first to limit peak USB current draw. */
static void startup_task(void *arg)
{
    (void)arg;

#if HAVE_BLE
    ble_hid_hold_controller_ram();
#endif

#if HAVE_WIFI_WEB
    wifi_manager_init();

    device_settings_t net_settings;
    settings_load(&net_settings);
    const bool home_mode = net_settings.network_mode == SETTINGS_NETWORK_HOME &&
                           (net_settings.wifi_ssid[0] != '\0' || net_settings.wifi_network_count > 0);

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

    health_check_init();
    start_http_server();
#else
    health_check_init();
#endif

#if HAVE_OLED
    display_init();
#endif

#if HAVE_BLE
#if HAVE_WIFI_WEB
    /* Portal first. httpd stacks go to PSRAM (ALWAYSINTERNAL≤1K); BLE keeps a
     * contiguous INTERNAL hold from early boot until pairing/reconnect. */
    if (home_mode && !wifi_manager_is_connected() && !wifi_manager_is_recovery_mode()) {
        ESP_LOGW(TAG, "Skipping BLE boot until network is reachable");
    } else {
        ble_hid_boot();
    }
#else
    ble_hid_boot();
#endif
#endif

#if HAVE_SDCARD
    esp_err_t sd_res = sd_card_mount_if_present();
    if (sd_res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(sd_res));
    }
#endif

#if CONFIG_BUDDY_NEO_LINK
    /* Applet sync is manual (`link install`) unless auto-install is enabled in menuconfig. */
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

#if HAVE_BLE
    /* Pin BLE controller RAM as early as possible — before USB host, SPIFFS,
     * Wi‑Fi, or httpd can fragment INTERNAL DRAM. */
    ble_hid_hold_controller_ram();
#endif

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiflash",
        .partition_label = "littlefs",
        .max_files = 8,
        .format_if_mount_failed = false
    };
    esp_err_t spiffs_res = esp_vfs_spiffs_register(&conf);
    if (spiffs_res == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiflash");
#if HAVE_WIFI_WEB
        struct stat portal_index;
        if (stat("/spiflash/index.html", &portal_index) != 0 &&
            stat("/spiflash/index.html.gz", &portal_index) != 0) {
            ESP_LOGE(TAG, "Portal index.html(.gz) missing from SPIFFS - web UI will not load");
        }
#endif
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(spiffs_res));
    }

    usb_host_neo_init();
    neo_debug_init();
    neo_autobackup_init();
#if CONFIG_BUDDY_NEO_LINK
    neo_link_applet_init();
#endif
    cloud_sync_init();
    if (neo_charmap_init() != ESP_OK) {
        ESP_LOGW(TAG, "Neo character maps failed to load; text import/export may be limited");
    }
    auth_init();

#if HAVE_BLE
    /* Re-assert hold after USB/storage init in case early pin failed. */
    ble_hid_hold_controller_ram();
#endif

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
