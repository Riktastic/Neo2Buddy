set(BUDDY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

idf_component_register(
    SRCS
        "test_main.c"
        "test_auth.c"
        "test_file_manager.c"
        "test_neo_message.c"
        "test_neo_import_text.c"
        "${BUDDY_ROOT}/services/auth.c"
        "${BUDDY_ROOT}/services/file_manager.c"
        "${BUDDY_ROOT}/services/device_status.c"
        "${BUDDY_ROOT}/neo/neo_message.c"
        "${BUDDY_ROOT}/neo/neo_import_text.c"
    INCLUDE_DIRS
        "."
        "${BUDDY_ROOT}/include"
        "${BUDDY_ROOT}/neo"
        "${BUDDY_ROOT}/services"
    REQUIRES unity nvs_flash esp_timer fatfs spiffs json driver sdmmc freertos
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_BUDDY_TEST_BUILD=1)
