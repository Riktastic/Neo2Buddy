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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "file_mgr";
static const char *SPIFFS_BACKUP_DIR = "/spiflash";
static const char *PROBE_NAME = "_buddy_probe.txt";

const char *file_manager_base_path(void)
{
#if HAVE_SDCARD
    if (sd_card_is_mounted()) {
        return NEO_IMPORT_DIRECTORY;
    }
#endif
    /* SPIFFS is flat (no real directories). Keep backups next to the portal files
     * and filter to *.txt in list/upload paths. */
    return SPIFFS_BACKUP_DIR;
}

bool file_manager_flash_ready(void)
{
    if (esp_spiffs_mounted("littlefs") || esp_spiffs_mounted(NULL)) {
        return true;
    }
    /* Mounted() can lag registration quirks; portal file or info() prove VFS is live. */
    struct stat st;
    if (stat("/spiflash/index.html", &st) == 0) {
        return true;
    }
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("littlefs", &total, &used) == ESP_OK) {
        return true;
    }
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        return true;
    }
    return false;
}

esp_err_t file_manager_ensure_dir(void)
{
    const char *base = file_manager_base_path();
    struct stat st;
    if (stat(base, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    /* SPIFFS is flat: the mount root is not a real directory for stat/mkdir. */
    if (strncmp(base, "/spiflash", 9) == 0) {
        if (file_manager_flash_ready()) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "SPIFFS partition not mounted");
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

esp_err_t file_manager_probe_backup_storage(char *detail, size_t detail_size)
{
    if (detail && detail_size) {
        detail[0] = '\0';
    }
    if (file_manager_ensure_dir() != ESP_OK) {
        if (detail && detail_size) {
            snprintf(detail, detail_size, "ensure_dir failed base=%s", file_manager_base_path());
        }
        return ESP_FAIL;
    }

    const char *payload = "buddy-storage-probe\n";
    const size_t payload_len = strlen(payload);
    esp_err_t err = file_manager_upload(PROBE_NAME, (const uint8_t *)payload, payload_len);
    if (err != ESP_OK) {
        if (detail && detail_size) {
            snprintf(detail, detail_size, "upload failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    char path[320];
    if (file_manager_resolve_path(PROBE_NAME, path, sizeof(path)) != ESP_OK) {
        file_manager_delete(PROBE_NAME);
        if (detail && detail_size) {
            snprintf(detail, detail_size, "resolve failed");
        }
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        file_manager_delete(PROBE_NAME);
        if (detail && detail_size) {
            snprintf(detail, detail_size, "fopen read failed errno=%d path=%s", errno, path);
        }
        return ESP_FAIL;
    }
    char buf[64];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    if (got != payload_len || memcmp(buf, payload, payload_len) != 0) {
        file_manager_delete(PROBE_NAME);
        if (detail && detail_size) {
            snprintf(detail, detail_size, "content mismatch got=%u", (unsigned)got);
        }
        return ESP_FAIL;
    }

    /* Confirm the file appears in a directory listing (Hammer/cloud list path). */
    bool listed = false;
    DIR *dir = opendir(file_manager_base_path());
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, PROBE_NAME) == 0) {
                listed = true;
                break;
            }
        }
        closedir(dir);
    }
    file_manager_delete(PROBE_NAME);
    if (!listed) {
        if (detail && detail_size) {
            snprintf(detail, detail_size, "wrote OK but opendir/list missed %s", PROBE_NAME);
        }
        return ESP_FAIL;
    }

    if (detail && detail_size) {
        snprintf(detail, detail_size, "ok base=%s flash_ready=%d", file_manager_base_path(),
                 file_manager_flash_ready() ? 1 : 0);
    }
    return ESP_OK;
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
    /* Mounted but info() unavailable — do not block small writes (probe/backup). */
    if (file_manager_flash_ready()) {
        return 256 * 1024;
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
        /* Flash root also holds portal assets — only list backup text files. */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcasecmp(ent->d_name + nlen - 4, ".txt") != 0) {
            continue;
        }
        if (file_manager_validate_name(ent->d_name) != ESP_OK) {
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
        /* Direct write fallback when .tmp open fails (flat SPIFFS edge cases). */
        f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "upload open failed %s errno=%d", path, errno);
            return ESP_FAIL;
        }
        if (fwrite(data, 1, length, f) != length || fflush(f) != 0) {
            fclose(f);
            unlink(path);
            return ESP_FAIL;
        }
        (void)fsync(fileno(f));
        fclose(f);
        ESP_LOGI(TAG, "uploaded %s (%u bytes, direct)", name, (unsigned)length);
        return ESP_OK;
    }
    if (fwrite(data, 1, length, f) != length || fflush(f) != 0) {
        fclose(f);
        unlink(tmp);
        return ESP_FAIL;
    }
    (void)fsync(fileno(f));
    fclose(f);
    unlink(path); /* SPIFFS rename often fails if destination exists */
    if (rename(tmp, path) != 0) {
        /* Fall back to copying into the final path. */
        FILE *src = fopen(tmp, "rb");
        FILE *dst = fopen(path, "wb");
        bool ok = false;
        if (src && dst) {
            uint8_t chunk[256];
            size_t n;
            ok = true;
            while ((n = fread(chunk, 1, sizeof(chunk), src)) > 0) {
                if (fwrite(chunk, 1, n, dst) != n) {
                    ok = false;
                    break;
                }
            }
            if (ok && fflush(dst) != 0) {
                ok = false;
            }
            if (ok) {
                (void)fsync(fileno(dst));
            }
        }
        if (src) {
            fclose(src);
        }
        if (dst) {
            fclose(dst);
        }
        unlink(tmp);
        if (!ok) {
            unlink(path);
            ESP_LOGE(TAG, "upload rename/copy failed %s errno=%d", path, errno);
            return ESP_FAIL;
        }
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
