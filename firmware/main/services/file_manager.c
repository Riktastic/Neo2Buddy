/**
 * @file file_manager.c
 * @brief Safe local document storage for the web portal (/api/v1/files).
 *
 * Validates filenames (no path traversal), stores under /sdcard/neo when SD is
 * mounted else /spiflash/neo. List/upload/delete/rename/download power the
 * backups panel. Neo import uses neo_import paths; this layer is generic UTF-8
 * file CRUD on the buddy filesystem.
 */

#include "file_manager.h"
#include "neo_import.h"
#include "board_config.h"
#if HAVE_SDCARD
#include "sd_card.h"
#endif
#include "device_status.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "file_mgr";
static const char *SPIFFS_BACKUP_DIR = "/spiflash/neo";

const char *file_manager_base_path(void)
{
#if HAVE_SDCARD
    if (sd_card_is_mounted()) {
        return NEO_IMPORT_DIRECTORY;
    }
#endif
    return SPIFFS_BACKUP_DIR;
}

esp_err_t file_manager_ensure_dir(void)
{
    const char *base = file_manager_base_path();
    struct stat st;
    if (stat(base, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    /* SPIFFS has no real directories; writing files under /spiflash/... works once mounted. */
    if (strncmp(base, "/spiflash", 9) == 0) {
        if (stat("/spiflash", &st) == 0) {
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    if (mkdir(base, 0755) == 0) {
        return ESP_OK;
    }
    if (errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "mkdir %s failed", base);
    return ESP_FAIL;
}

esp_err_t file_manager_validate_name(const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) >= FILE_MANAGER_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (name[0] == '.' || strchr(name, '/') || strchr(name, '\\')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(name, "..")) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t file_manager_resolve_path(const char *name, char *out, size_t out_size)
{
    esp_err_t err = file_manager_validate_name(name);
    if (err != ESP_OK) {
        return err;
    }
    const char *base = file_manager_base_path();
    int n = snprintf(out, out_size, "%s/%s", base, name);
    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

size_t file_manager_free_bytes(void)
{
    const char *base = file_manager_base_path();
#if HAVE_SDCARD
    if (sd_card_is_mounted()) {
        uint64_t total = 0;
        uint64_t used = 0;
        if (esp_vfs_fat_info(base, &total, &used) == ESP_OK && total > used) {
            return (size_t)(total - used);
        }
        device_status_t st;
        device_status_get(&st);
        if (st.sd_card_total_bytes > st.sd_card_used_bytes) {
            return st.sd_card_total_bytes - st.sd_card_used_bytes;
        }
        return 0;
    }
#endif
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("littlefs", &total, &used) == ESP_OK && total > used) {
        return total - used;
    }
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK && total > used) {
        return total - used;
    }
    return 0;
}

esp_err_t file_manager_list(cJSON *array)
{
    if (!array) {
        return ESP_ERR_INVALID_ARG;
    }
    if (file_manager_ensure_dir() != ESP_OK) {
        return ESP_FAIL;
    }

    const char *base = file_manager_base_path();
    DIR *dir = opendir(base);
    if (!dir) {
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", base, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "name", ent->d_name);
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        cJSON_AddNumberToObject(item, "modified", (double)st.st_mtime);
        cJSON_AddItemToArray(array, item);
    }
    closedir(dir);
    return ESP_OK;
}

esp_err_t file_manager_upload(const char *name, const uint8_t *data, size_t length)
{
    esp_err_t err = file_manager_validate_name(name);
    if (err != ESP_OK || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0 || length > FILE_MANAGER_MAX_UPLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (length > file_manager_free_bytes()) {
        return ESP_ERR_NO_MEM;
    }
    if (file_manager_ensure_dir() != ESP_OK) {
        return ESP_FAIL;
    }

    char path[320];
    err = file_manager_resolve_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    char tmp[336];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    if (fwrite(data, 1, length, f) != length) {
        fclose(f);
        unlink(tmp);
        return ESP_FAIL;
    }
    fclose(f);
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "uploaded %s (%u bytes)", name, (unsigned)length);
    return ESP_OK;
}

esp_err_t file_manager_delete(const char *name)
{
    char path[320];
    esp_err_t err = file_manager_resolve_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    if (unlink(path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t file_manager_rename(const char *old_name, const char *new_name)
{
    char old_path[320];
    char new_path[320];
    esp_err_t err = file_manager_resolve_path(old_name, old_path, sizeof(old_path));
    if (err != ESP_OK) {
        return err;
    }
    err = file_manager_validate_name(new_name);
    if (err != ESP_OK) {
        return err;
    }
    err = file_manager_resolve_path(new_name, new_path, sizeof(new_path));
    if (err != ESP_OK) {
        return err;
    }
    struct stat st;
    if (stat(new_path, &st) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(old_path, new_path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
