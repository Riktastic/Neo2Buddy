/**
 * @file self_test.c
 * @brief Lightweight on-device smoke tests run once at boot.
 */

#include "self_test.h"
#include "neo_message.h"
#include "file_manager.h"
#include "device_status.h"
#include "auth.h"
#include "log_buffer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "self_test";

static uint32_t s_failures = 0;

static void expect_true(const char *name, bool condition)
{
    if (condition) {
        ESP_LOGI(TAG, "PASS: %s", name);
    } else {
        ESP_LOGE(TAG, "FAIL: %s", name);
        log_buffer_appendf("self_test: FAIL %s", name);
        s_failures++;
    }
}

uint32_t self_test_run(void)
{
    s_failures = 0;
    ESP_LOGI(TAG, "Starting boot self-test");

    /* NEO message framing */
    neo_message_t msg;
    const uint32_t args[][3] = { {0x0102, 1, 2}, {0, 0, 0} };
    neo_message_init(&msg, 0x42, args);
    expect_true("neo_message command", neo_message_command(&msg) == 0x42);
    expect_true("neo_message checksum", neo_message_checksum_is_valid(&msg));
    expect_true("neo_message argument", neo_message_argument(&msg, 1, 2) == 0x0102);

    /* File name safety */
    expect_true("file name ok", file_manager_validate_name("notes.txt") == ESP_OK);
    expect_true("file name rejects traversal", file_manager_validate_name("../secret") == ESP_ERR_INVALID_ARG);
    expect_true("file name rejects slash", file_manager_validate_name("a/b") == ESP_ERR_INVALID_ARG);

    /* Device status snapshot */
    device_status_t st;
    device_status_get(&st);
    expect_true("device_status readable", st.wifi_state == DEVICE_WIFI_UNCONFIGURED ||
                                          st.wifi_state == DEVICE_WIFI_CONNECTING ||
                                          st.wifi_state == DEVICE_WIFI_CONNECTED ||
                                          st.wifi_state == DEVICE_WIFI_ERROR);

    /* Auth round-trip (uses NVS; non-destructive to existing session if login succeeds) */
    char token[65];
    esp_err_t login = auth_login("neo2buddy", token, sizeof(token));
    if (login == ESP_OK) {
        expect_true("auth token issued", strlen(token) == 64);
        expect_true("auth token valid", auth_check_token(token));
        auth_logout();
        expect_true("auth logout clears token", !auth_check_token(token));
    } else {
        ESP_LOGW(TAG, "auth login skipped (custom password or rate limit)");
        log_buffer_appendf("self_test: auth login skipped");
    }

    /* Backup storage round-trip (SPIFFS or SD) — same path autobackup/Hammer use */
    {
        char detail[128];
        esp_err_t probe = file_manager_probe_backup_storage(detail, sizeof(detail));
        expect_true("backup storage probe", probe == ESP_OK);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "storage probe: %s", detail);
            log_buffer_appendf("self_test: storage %s", detail);
        } else {
            ESP_LOGE(TAG, "storage probe failed: %s", detail[0] ? detail : esp_err_to_name(probe));
            log_buffer_appendf("self_test: storage FAIL %s", detail[0] ? detail : esp_err_to_name(probe));
        }
    }

    if (s_failures == 0) {
        ESP_LOGI(TAG, "Boot self-test passed");
        log_buffer_appendf("self_test: all checks passed");
    } else {
        ESP_LOGW(TAG, "Boot self-test failures: %u", (unsigned)s_failures);
        log_buffer_appendf("self_test: %u failure(s)", (unsigned)s_failures);
    }

    return s_failures;
}
