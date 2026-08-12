cmake_minimum_required(VERSION 3.16)

set(buddy_srcs
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
    "services/log_buffer.c"
    "services/file_manager.c"
    "services/hammer_ink.c"
    "services/factory_reset.c"
    "services/self_test.c"
)

set(buddy_requires
    usb
    nvs_flash
    esp_netif
    esp_event
    esp_timer
    mbedtls
    spiffs
    fatfs
    json
    driver
    esp_adc
    console
    # Always declare optional stacks here. Early component-requirements expansion
    # runs before sdkconfig is loaded, so if(CONFIG_SUPPORT_*) cannot gate REQUIRES.
    esp_wifi
    esp_http_server
    esp_http_client
    bt
    esp_hid
    sdmmc
    esp_driver_spi
)

if(CONFIG_SUPPORT_WIFI_WEB)
    list(APPEND buddy_srcs
        "services/wifi_manager.c"
        "services/captive_dns.c"
        "services/cloud_sync.c"
        "web/web_api_http.c"
    )
else()
    list(APPEND buddy_srcs
        "services/wifi_manager_stub.c"
        "services/cloud_sync_stub.c"
    )
endif()

if(CONFIG_SUPPORT_BLE)
    list(APPEND buddy_srcs
        "services/ble_hid.c"
        "services/ble_hid_device.c"
        "services/ble_hid_gatt.c"
    )
else()
    list(APPEND buddy_srcs "services/ble_hid_stub.c")
endif()

if(CONFIG_SUPPORT_STOCK_APPLETS)
    list(APPEND buddy_srcs
        "services/stock_applets.c"
        "services/flash_decks.c"
    )
endif()

if(CONFIG_SUPPORT_BATTERY)
    list(APPEND buddy_srcs "services/battery.c")
    list(APPEND buddy_srcs "services/battery_monitor.c")
endif()

if(CONFIG_SUPPORT_SDCARD)
    list(APPEND buddy_srcs "services/sd_card.c")
    list(APPEND buddy_srcs "services/sd_format.c")
endif()

if(CONFIG_SUPPORT_OLED)
    list(APPEND buddy_srcs "services/display.c")
endif()

if(CONFIG_BUDDY_UART_CMD)
    list(APPEND buddy_srcs "services/uart_cmd.c")
endif()

if(CONFIG_BUDDY_NEO_LINK)
    # Abandoned feature. Copy archive/neo-link/firmware-buddy/* into neo/ first;
    # still will not get live applet UI over USB keyboard mode.
    list(APPEND buddy_srcs
        "neo/neo_link_transport.c"
        "neo/neo_link_mailbox.c"
        "neo/neo_link_llm.c"
        "neo/neo_link_text.c"
        "neo/neo_link_applet.c"
        "${CMAKE_SOURCE_DIR}/../archive/neo-link/neo-link-tree/protocol/neo_link_protocol.c"
    )
endif()

# Stock App Store applets (built by tools/build-stock-applets.ps1)
set(STOCK_EMBED_FILES "")
if(CONFIG_SUPPORT_STOCK_APPLETS)
    set(STOCK_APPS DiceTable TaskPad ScriptPad WordTree TypingDrill FlashCards MathDrill Snake HangWord TicTacToe TouchType)
    foreach(app ${STOCK_APPS})
        set(_stock "${CMAKE_CURRENT_LIST_DIR}/neo/embedded/${app}.OS3KApp")
        if(EXISTS "${_stock}")
            list(APPEND STOCK_EMBED_FILES "neo/embedded/${app}.OS3KApp")
            message(STATUS "Stock applet embedded: ${app}.OS3KApp")
        else()
            message(WARNING "Stock applet missing (run tools/build-stock-applets.ps1): ${app}.OS3KApp")
        endif()
    endforeach()
endif()

message(STATUS "Neo2 Buddy REQUIRES: ${buddy_requires}")

idf_component_register(
    SRCS ${buddy_srcs}
    INCLUDE_DIRS "include" "neo" "services" "web"
    REQUIRES ${buddy_requires}
    EMBED_FILES
        "neo/character_map/en-us.txt"
        "neo/character_map/ua-pc.txt"
        "neo/character_map/ua-mac.txt"
        ${STOCK_EMBED_FILES}
)

if(CONFIG_BUDDY_NEO_LINK)
    # Research-only path; applet binary lives under archive/neo-link/artifacts/.
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_SOURCE_DIR}/../archive/neo-link/neo-link-tree/protocol")
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_SOURCE_DIR}/../archive/neo-link/artifacts/NeoLinkChat.OS3KApp" BINARY)
endif()

# Pack portal UI into SPIFFS (or a minimal placeholder when Wi-Fi/web is off).
set(FIRMWARE_WEB_SRC "${CMAKE_SOURCE_DIR}/../firmware-web")
set(FIRMWARE_WEB_PACKED "${CMAKE_BINARY_DIR}/firmware-web-packed")
set(PACK_WEB_SCRIPT "${CMAKE_SOURCE_DIR}/../tools/pack_firmware_web.py")
set(PACK_MINIMAL_SCRIPT "${CMAKE_SOURCE_DIR}/../tools/pack_minimal_spiffs.py")
idf_build_get_property(python PYTHON)

if(CONFIG_SUPPORT_WIFI_WEB)
    file(GLOB_RECURSE FIRMWARE_WEB_FILES CONFIGURE_DEPENDS
        "${FIRMWARE_WEB_SRC}/*.html"
        "${FIRMWARE_WEB_SRC}/*.css"
        "${FIRMWARE_WEB_SRC}/*.js"
    )
    add_custom_command(
        OUTPUT "${FIRMWARE_WEB_PACKED}/.stamp"
        COMMAND "${python}" "${PACK_WEB_SCRIPT}" "${FIRMWARE_WEB_SRC}" "${FIRMWARE_WEB_PACKED}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${FIRMWARE_WEB_PACKED}/.stamp"
        DEPENDS "${PACK_WEB_SCRIPT}" ${FIRMWARE_WEB_FILES}
        COMMENT "Packing firmware-web (gzip large assets)"
        VERBATIM
    )
else()
    add_custom_command(
        OUTPUT "${FIRMWARE_WEB_PACKED}/.stamp"
        COMMAND "${python}" "${PACK_MINIMAL_SCRIPT}" "${FIRMWARE_WEB_PACKED}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${FIRMWARE_WEB_PACKED}/.stamp"
        DEPENDS "${PACK_MINIMAL_SCRIPT}"
        COMMENT "Packing minimal SPIFFS (UART-only profile)"
        VERBATIM
    )
endif()

add_custom_target(firmware_web_packed DEPENDS "${FIRMWARE_WEB_PACKED}/.stamp")
spiffs_create_partition_image(littlefs "${FIRMWARE_WEB_PACKED}" FLASH_IN_PROJECT DEPENDS firmware_web_packed)
