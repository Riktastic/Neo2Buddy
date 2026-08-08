/**
 * @file factory_reset.c
 * @brief Factory reset: NVS wipe + internal backup deletion, then reboot.
 */

#include "factory_reset.h"

#include "auth.h"
#include "ble_hid.h"
#include "log_buffer.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "factory_reset";

#define FACTORY_RESET_SPIFFS_BACKUP_DIR "/spiflash/neo"

static esp_err_t factory_reset_erase_namespace(const char *ns)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void factory_reset_delete_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return;
    }
    if (unlink(path) == 0) {
        ESP_LOGI(TAG, "deleted %s", path);
        return;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }
    char child[280];
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        factory_reset_delete_path(child);
    }
    closedir(dir);
    unlink(path);
}

static esp_err_t factory_reset_clear_internal_backups(void)
{
    factory_reset_delete_path(FACTORY_RESET_SPIFFS_BACKUP_DIR);

    DIR *root = opendir("/spiflash");
    if (!root) {
        return ESP_OK;
    }

    char path[280];
    struct dirent *ent;
    while ((ent = readdir(root)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (strncmp(ent->d_name, "neo", 3) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/spiflash/%s", ent->d_name);
        factory_reset_delete_path(path);
    }
    closedir(root);
    return ESP_OK;
}

static esp_err_t factory_reset_restore_defaults(void)
{
    device_settings_t settings;
    settings_defaults(&settings);
    settings_apply_hotspot_defaults(&settings);
    return settings_save(&settings);
}

void factory_reset_execute(void)
{
    ESP_LOGW(TAG, "Factory reset executing");
    log_buffer_append_level(LOG_LEVEL_WARN, "factory reset started");

    esp_err_t result = factory_reset_clear_internal_backups();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "internal backup cleanup: %s", esp_err_to_name(result));
    }

    const char *namespaces[] = {"device", "auth", "cloud_sync", "nimble_bond", "bt_nimble"};
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        result = factory_reset_erase_namespace(namespaces[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "erase NVS %s failed: %s", namespaces[i], esp_err_to_name(result));
        }
    }

    ble_hid_clear_bonds();

    result = factory_reset_restore_defaults();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "restore defaults failed: %s", esp_err_to_name(result));
    }

    (void)auth_logout();
    log_buffer_append_level(LOG_LEVEL_WARN, "factory reset complete — rebooting");
    ESP_LOGW(TAG, "Factory reset complete, rebooting");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

esp_err_t factory_reset_perform(const char *password, char *err, size_t err_size)
{
    if (!password || password[0] == '\0') {
        if (err && err_size > 0) {
            snprintf(err, err_size, "password required");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (auth_login_rate_limited()) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "too many attempts — wait 60s");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!auth_check_password(password)) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "invalid password");
        }
        return ESP_ERR_INVALID_ARG;
    }

    factory_reset_execute();
    return ESP_OK;
}
