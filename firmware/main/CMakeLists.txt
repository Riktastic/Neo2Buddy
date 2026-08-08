cmake_minimum_required(VERSION 3.16)

set(srcs
    "main.c"
    "neo/neo_message.c"
    "neo/neo_device.c"
    "neo/neo_debug.c"
    "neo/neo_applet.c"
    "neo/neo_import.c"
    "neo/neo_import_text.c"
    "neo/neo_autobackup.c"
    "neo/neo_conv.c"
    "neo/neo_space.c"
    "neo/neo_settings.c"
    "neo/usb_host_neo.c"
    "neo/neo_file.c"
    "neo/neo_file_extra.c"
    "neo/neo_usb_hid.c"
    "services/auth.c"
    "services/hid_debug.c"
    "services/health_check.c"
    "services/device_status.c"
    "services/neo_live.c"
    "services/settings.c"
    "services/wifi_manager.c"
    "services/captive_dns.c"
    "services/log_buffer.c"
    "services/file_manager.c"
    "services/cloud_sync.c"
    "services/factory_reset.c"
    "services/self_test.c"
    "services/ble_hid.c"
    "services/ble_hid_device.c"
    "services/ble_hid_gatt.c"
    "web/web_api_http.c"
)

if(CONFIG_SUPPORT_BATTERY)
    list(APPEND srcs "services/battery.c")
    list(APPEND srcs "services/battery_monitor.c")
endif()

if(CONFIG_SUPPORT_SDCARD)
    list(APPEND srcs "services/sd_card.c")
    list(APPEND srcs "services/sd_format.c")
endif()

if(CONFIG_SUPPORT_OLED)
    list(APPEND srcs "services/display.c")
endif()

if(CONFIG_BUDDY_UART_CMD)
    list(APPEND srcs "services/uart_cmd.c")
endif()

set(requires
    usb
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_http_server
    esp_http_client
    esp_timer
    mbedtls
    spiffs
    fatfs
    json
    driver
    esp_adc
    sdmmc
    esp_driver_spi
    bt
    console
    mdns
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "neo" "services" "web"
    REQUIRES ${requires}
    EMBED_FILES
        "neo/character_map/en-us.txt"
        "neo/character_map/ua-pc.txt"
        "neo/character_map/ua-mac.txt"
)

# Pack the repo firmware-web/ portal into the SPIFFS partition labelled "littlefs".
spiffs_create_partition_image(littlefs "${CMAKE_SOURCE_DIR}/../firmware-web" FLASH_IN_PROJECT)
