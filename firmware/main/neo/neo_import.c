/**
 * @file neo_import.c
 * @brief Persist Neo documents as UTF-8 `.txt` backups on SD or internal flash.
 *
 * Pipeline (AlphaWord):
 *   Neo raw bytes → neo_conv UTF-8 → content policy checks → prune if needed →
 *   atomic write under /sdcard/neo (preferred) or flat files on /spiflash
 *   (SPIFFS has no real directories — do not mkdir /spiflash/neo).
 *
 * Filename scheme (see neo_import.h):
 *   {deviceId}_s{NN}_{fileName}_{YYYYMMDD}.txt
 *   - deviceId = neo_label, else Buddy device_name (classroom label; Neo has no MAC).
 *   - sNN keeps AlphaWord slots distinct when Neo names collide or are empty.
 *   - YYYYMMDD (or "nodate" before SNTP) versions by calendar day without a DB.
 *
 * Content policy (production):
 *   - Skip blank / whitespace-only text (spaces, tabs, CR, LF only).
 *     Blank check lives in neo_import_text.c (neo_import_text_is_blank).
 *   - Skip when any existing `.txt` in the backup dir already has identical bytes
 *     (not only today's dated path — avoids re-saving under a new stamp).
 *   - Before writing, prune oldest backups if free space or file count is low.
 *     Flash limits are tighter than SD; factory reset never deletes SD files.
 *
 * Tried and rejected: Wi-Fi MAC as deviceId (labels the ESP board, not which Neo).
 * See firmware/docs/neo-usb-and-backup.md.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "neo_import.h"
#include "file_manager.h"
#include "board_config.h"
#if HAVE_SDCARD
#include "sd_card.h"
#include "esp_vfs_fat.h"
#endif
#include "neo_conv.h"
#include "neo_file.h"
#include "neo_applet.h"
#include "settings.h"

static const char *TAG = "neo_import";

/* Internal flash shares LittleFS with the portal SPA — keep headroom. */
#define NEO_IMPORT_FLASH_MIN_FREE (96 * 1024)
#define NEO_IMPORT_FLASH_MAX_FILES 48
/* SD is larger; still cap count so listings stay manageable. */
#define NEO_IMPORT_SD_MIN_FREE (256 * 1024)
#define NEO_IMPORT_SD_MAX_FILES 200

/* -------------------------------------------------------------------------- */
/* Path helpers                                                                */
/* -------------------------------------------------------------------------- */

/**
 * Map Neo / user strings to safe path components: keep alnum/_/-, turn space/dot
 * into '_', drop everything else. Empty result becomes "untitled".
 */
static void sanitize_name(const char *source, char *destination, size_t destination_size)
{
    size_t output_length = 0;
    for (size_t index = 0; source != NULL && source[index] != '\0' && output_length + 1 < destination_size;
         index++) {
        unsigned char character = (unsigned char)source[index];
        if (isalnum(character) || character == '-' || character == '_') {
            destination[output_length++] = (char)character;
        } else if (character == ' ' || character == '.') {
            destination[output_length++] = '_';
        }
    }

    if (output_length == 0) {
        strlcpy(destination, "untitled", destination_size);
        return;
    }
    destination[output_length] = '\0';
}

/** Filename prefix: neo_label preferred so one buddy can label multiple Neos. */
static void fill_device_id(char *out, size_t out_size)
{
    const char *name = settings_get_backup_label();
    sanitize_name(name, out, out_size);
    if (out[0] == '\0') {
        strlcpy(out, "Neo2Buddy", out_size);
    }
}

/**
 * Calendar stamp for the filename. Before NTP/SNTP the RTC is wrong — use
 * "nodate" so we never invent a fake day that later collides with real dates.
 */
static void fill_date_stamp(char *out, size_t out_size)
{
    time_t now = time(NULL);
    if (now < (time_t)1609459200) { /* 2021-01-01 — treat earlier as unset */
        strlcpy(out, "nodate", out_size);
        return;
    }
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) == NULL) {
        strlcpy(out, "nodate", out_size);
        return;
    }
    if (strftime(out, out_size, "%Y%m%d", &tm_now) == 0) {
        strlcpy(out, "nodate", out_size);
    }
}

/** Prefer microSD when mounted; otherwise flat SPIFFS under /spiflash (no mkdir). */
const char *neo_import_base_dir(void)
{
#if HAVE_SDCARD
    if (sd_card_is_mounted()) {
        return NEO_IMPORT_DIRECTORY;
    }
#endif
    return "/spiflash";
}

/**
 * Build today's canonical path for a slot without writing.
 * Callers use this for “unchanged today?” checks; full-history dedupe uses
 * neo_import_matching_backup_exists() across all .txt files in the dir.
 */
esp_err_t neo_import_build_document_path(uint8_t file_index, const char *file_name,
                                         char *out_path, size_t out_path_size)
{
    if (!out_path || out_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char device_id[NEO_IMPORT_DEVICE_ID_MAX];
    char sanitized_name[NEO_DOCUMENT_NAME_MAX_LENGTH + 1];
    char date_stamp[16];
    fill_device_id(device_id, sizeof(device_id));
    sanitize_name(file_name, sanitized_name, sizeof(sanitized_name));
    fill_date_stamp(date_stamp, sizeof(date_stamp));

    const char *base = neo_import_base_dir();
    const bool on_flash = strncmp(base, "/spiflash", 9) == 0;
    int n;
    if (on_flash) {
        /* Keep under SPIFFS object-name limits: short device + name + date. */
        n = snprintf(out_path, out_path_size, "%s/%.10s_s%02u_%.16s_%.8s.txt", base, device_id,
                     file_index, sanitized_name, date_stamp);
    } else {
        n = snprintf(out_path, out_path_size, "%s/%s_s%02u_%s_%s.txt", base, device_id, file_index,
                     sanitized_name, date_stamp);
    }
    if (n < 0 || (size_t)n >= out_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Content compare & storage policy                                            */
/* -------------------------------------------------------------------------- */

/**
 * Byte-for-byte compare of an on-disk file to UTF-8 text.
 * Size mismatch fails fast; otherwise read in 256-byte chunks (low stack).
 */
bool neo_import_file_matches(const char *path, const char *text, size_t text_len)
{
    if (!path || !text) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz != text_len) {
        fclose(f);
        return false;
    }
    if (text_len == 0) {
        fclose(f);
        return true;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t chunk[256];
    size_t offset = 0;
    while (offset < text_len) {
        size_t n = text_len - offset;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (fread(chunk, 1, n, f) != n) {
            fclose(f);
            return false;
        }
        if (memcmp(chunk, text + offset, n) != 0) {
            fclose(f);
            return false;
        }
        offset += n;
    }
    fclose(f);
    return true;
}

/**
 * True if any `.txt` in the backup directory already holds this exact content.
 * Prevents filling flash with dated copies of the same AlphaWord document.
 */
bool neo_import_matching_backup_exists(const char *text, size_t text_len)
{
    if (!text) {
        return false;
    }
    const char *base = neo_import_base_dir();
    DIR *dir = opendir(base);
    if (!dir) {
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcasecmp(ent->d_name + nlen - 4, ".txt") != 0) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", base, ent->d_name);
        if (neo_import_file_matches(path, text, text_len)) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

/** Free bytes on the active backup volume (FAT info on SD, SPIFFS info on flash). */
static size_t neo_import_storage_free_bytes(void)
{
    const char *base = neo_import_base_dir();
#if HAVE_SDCARD
    if (strncmp(base, "/sdcard", 7) == 0) {
        uint64_t total = 0;
        uint64_t used = 0;
        if (esp_vfs_fat_info("/sdcard", &total, &used) == ESP_OK && total > used) {
            return (size_t)(total - used);
        }
        return 0;
    }
#endif
    size_t total = 0;
    size_t used = 0;
    /* Partition label for the portal image is "littlefs"; fall back to default. */
    if (esp_spiffs_info("littlefs", &total, &used) == ESP_OK && total > used) {
        return total - used;
    }
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK && total > used) {
        return total - used;
    }
    return 0;
}

typedef struct {
    char name[64];
    time_t mtime;
    size_t size;
} neo_import_file_meta_t;

/** Oldest mtime first so prune deletes the least-recent backups. */
static int neo_import_meta_cmp(const void *a, const void *b)
{
    const neo_import_file_meta_t *fa = a;
    const neo_import_file_meta_t *fb = b;
    if (fa->mtime < fb->mtime) {
        return -1;
    }
    if (fa->mtime > fb->mtime) {
        return 1;
    }
    return strcmp(fa->name, fb->name);
}

/**
 * Delete oldest `.txt` backups until free space and file-count targets are met.
 * @param bytes_needed Extra free space to reserve for the upcoming write.
 * @return Number of files removed (0 if already healthy or dir missing).
 */
size_t neo_import_prune_old_backups(size_t bytes_needed)
{
    const char *base = neo_import_base_dir();
    const bool on_flash = strncmp(base, "/spiflash", 9) == 0;
    const size_t min_free = on_flash ? NEO_IMPORT_FLASH_MIN_FREE : NEO_IMPORT_SD_MIN_FREE;
    const size_t max_files = on_flash ? NEO_IMPORT_FLASH_MAX_FILES : NEO_IMPORT_SD_MAX_FILES;
    const size_t target_free = min_free + bytes_needed;

    DIR *dir = opendir(base);
    if (!dir) {
        return 0;
    }

    neo_import_file_meta_t *files = calloc(max_files + 64, sizeof(*files));
    if (!files) {
        closedir(dir);
        return 0;
    }
    size_t count = 0;
    size_t capacity = max_files + 64;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcasecmp(ent->d_name + nlen - 4, ".txt") != 0) {
            continue;
        }
        if (count >= capacity) {
            break;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", base, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        strlcpy(files[count].name, ent->d_name, sizeof(files[count].name));
        files[count].mtime = st.st_mtime;
        files[count].size = (size_t)st.st_size;
        count++;
    }
    closedir(dir);

    if (count == 0) {
        free(files);
        return 0;
    }

    qsort(files, count, sizeof(*files), neo_import_meta_cmp);

    size_t removed = 0;
    size_t idx = 0;
    while (idx < count) {
        size_t free_now = neo_import_storage_free_bytes();
        size_t remaining = count - removed;
        if (free_now >= target_free && remaining <= max_files) {
            break;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", base, files[idx].name);
        if (unlink(path) == 0) {
            ESP_LOGW(TAG, "pruned old backup %s (%u bytes free needed)", files[idx].name,
                     (unsigned)bytes_needed);
            removed++;
        }
        idx++;
    }

    free(files);
    return removed;
}

/* -------------------------------------------------------------------------- */
/* Atomic write & public save API                                              */
/* -------------------------------------------------------------------------- */

/**
 * Direct overwrite (SPIFFS-safe fallback when rename is unreliable).
 */
static esp_err_t write_file_direct(const char *destination_path, const char *contents,
                                   size_t contents_length)
{
    FILE *file = fopen(destination_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Could not open document: %s errno=%d", destination_path, errno);
        return ESP_FAIL;
    }
    size_t written = fwrite(contents, 1, contents_length, file);
    if (written != contents_length || fflush(file) != 0) {
        fclose(file);
        unlink(destination_path);
        ESP_LOGE(TAG, "Could not write AlphaSmart document: errno=%d", errno);
        return ESP_FAIL;
    }
    (void)fsync(fileno(file)); /* best-effort; SPIFFS may return EINVAL */
    fclose(file);
    return ESP_OK;
}

/**
 * Write via `.tmp` + fflush + rename so a power cut does not leave a half file
 * as the only copy of a Neo document. Falls back to direct write on SPIFFS
 * rename quirks (destination exists, or rename unsupported).
 */
static esp_err_t write_file_atomically(const char *destination_path, const char *contents,
                                       size_t contents_length)
{
    char temporary_path[288];
    int characters_written = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", destination_path);
    if (characters_written < 0 || (size_t)characters_written >= sizeof(temporary_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL) {
        ESP_LOGW(TAG, "tmp open failed errno=%d; trying direct write", errno);
        return write_file_direct(destination_path, contents, contents_length);
    }

    size_t written = fwrite(contents, 1, contents_length, file);
    if (written != contents_length || fflush(file) != 0) {
        fclose(file);
        unlink(temporary_path);
        ESP_LOGE(TAG, "Could not write AlphaSmart document: errno=%d", errno);
        return ESP_FAIL;
    }
    (void)fsync(fileno(file)); /* best-effort; do not fail the backup on fsync alone */
    fclose(file);

    unlink(destination_path); /* SPIFFS rename often fails if dest already exists */
    if (rename(temporary_path, destination_path) != 0) {
        ESP_LOGW(TAG, "rename failed errno=%d; copying to final path", errno);
        esp_err_t err = write_file_direct(destination_path, contents, contents_length);
        unlink(temporary_path);
        return err;
    }
    return ESP_OK;
}

/**
 * Write UTF-8 bytes to @p destination_path (mkdir/prune handled by caller).
 */
static esp_err_t write_prepared_document(const char *destination_path, const char *contents,
                                         size_t contents_length)
{
    if (!destination_path || !contents) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_file_atomically(destination_path, contents, contents_length);
}

static esp_err_t prepare_backup_directory(size_t bytes_needed)
{
    const char *base_dir = neo_import_base_dir();
    /* SPIFFS is flat: mkdir and stat("/spiflash") both fail even when mounted. */
    if (strncmp(base_dir, "/spiflash", 9) == 0) {
        if (!file_manager_flash_ready()) {
            ESP_LOGE(TAG, "SPIFFS partition not ready at %s", base_dir);
            return ESP_FAIL;
        }
    } else if (mkdir(base_dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Could not create Neo directory: %s errno=%d", base_dir, errno);
        return ESP_FAIL;
    }
    (void)neo_import_prune_old_backups(bytes_needed);
    return ESP_OK;
}

/**
 * Save one UTF-8 document after policy checks.
 * @return ESP_OK on write;
 *         ESP_ERR_NOT_FOUND if blank (skipped);
 *         ESP_ERR_INVALID_STATE if a duplicate already exists (skipped).
 */
esp_err_t neo_import_save_document(const neo_document_t *document, char *saved_path,
                                   size_t saved_path_size)
{
    if (document == NULL || document->utf8_text == NULL || saved_path == NULL || saved_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (neo_import_text_is_blank(document->utf8_text, document->utf8_text_length)) {
        ESP_LOGI(TAG, "skip blank document index=%u", document->file_index);
        return ESP_ERR_NOT_FOUND;
    }
    if (neo_import_matching_backup_exists(document->utf8_text, document->utf8_text_length)) {
        ESP_LOGI(TAG, "skip duplicate content index=%u", document->file_index);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t prep = prepare_backup_directory(document->utf8_text_length + 4096);
    if (prep != ESP_OK) {
        return prep;
    }

    esp_err_t err = neo_import_build_document_path(document->file_index, document->file_name, saved_path,
                                                 saved_path_size);
    if (err != ESP_OK) {
        return err;
    }

    return write_prepared_document(saved_path, document->utf8_text, document->utf8_text_length);
}

/**
 * Save when today's canonical path already exists but holds different bytes.
 * Skips only when @p destination_path exists and its contents match exactly.
 */
esp_err_t neo_import_save_document_if_changed(const neo_document_t *document, char *saved_path,
                                              size_t saved_path_size)
{
    if (document == NULL || document->utf8_text == NULL || saved_path == NULL || saved_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (neo_import_text_is_blank(document->utf8_text, document->utf8_text_length)) {
        ESP_LOGI(TAG, "skip blank document index=%u", document->file_index);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = neo_import_build_document_path(document->file_index, document->file_name, saved_path,
                                                 saved_path_size);
    if (err != ESP_OK) {
        return err;
    }
    if (neo_import_file_matches(saved_path, document->utf8_text, document->utf8_text_length)) {
        ESP_LOGI(TAG, "skip unchanged content index=%u", document->file_index);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t prep = prepare_backup_directory(document->utf8_text_length + 4096);
    if (prep != ESP_OK) {
        return prep;
    }

    return write_prepared_document(saved_path, document->utf8_text, document->utf8_text_length);
}

/**
 * Force-write today's path (Backup all). Blank still skipped; identical existing
 * backups do not block the write.
 */
esp_err_t neo_import_save_document_force(const neo_document_t *document, char *saved_path,
                                         size_t saved_path_size)
{
    if (document == NULL || document->utf8_text == NULL || saved_path == NULL || saved_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (neo_import_text_is_blank(document->utf8_text, document->utf8_text_length)) {
        ESP_LOGI(TAG, "skip blank document index=%u", document->file_index);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = neo_import_build_document_path(document->file_index, document->file_name, saved_path,
                                                 saved_path_size);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t prep = prepare_backup_directory(document->utf8_text_length + 4096);
    if (prep != ESP_OK) {
        return prep;
    }

    return write_prepared_document(saved_path, document->utf8_text, document->utf8_text_length);
}

/**
 * Convert Neo raw applet bytes to UTF-8 (EN-US map) then save via the same
 * policy path as AlphaWord text. Used for non-AlphaWord applet file downloads.
 */
esp_err_t neo_import_save_raw_document(const uint8_t *neo_data, size_t neo_len,
                                       const char *file_name, uint8_t file_index, char *saved_path,
                                       size_t saved_path_size)
{
    if (!neo_data || !file_name || !saved_path) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t buf_size = neo_conv_export_buf_size(neo_len);
    char *buf = malloc(buf_size);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t written = neo_conv_export_text_from_neo(neo_data, neo_len, buf, buf_size, NEO_CHARMAP_EN_US);
    if (written == 0) {
        free(buf);
        return ESP_FAIL;
    }

    neo_document_t doc = {
        .file_index = file_index,
        .file_name = file_name,
        .utf8_text = buf,
        .utf8_text_length = written,
    };

    esp_err_t res = neo_import_save_document(&doc, saved_path, saved_path_size);
    free(buf);
    return res;
}

/**
 * NeoTools-style “read all files on applet”: walk indices until NOT_FOUND.
 * AlphaWord slots are exported with @p map; other applets use raw→UTF-8 via EN-US.
 * Yields once per file so USB bulk callbacks stay responsive.
 * Blank slots are skipped; non-empty AlphaWord files are force-written (Backup all).
 */
esp_err_t neo_import_backup_applet_files(uint16_t applet_id, neo_charmap_id_t map,
                                         neo_import_saved_file_t *results, size_t max_results,
                                         size_t *out_count)
{
    if (!results || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    for (uint8_t index = 1; index != 0 && *out_count < max_results; ++index) {
        vTaskDelay(1);

        neo_file_attr_t attrs;
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        esp_err_t err = neo_file_read_alloc(applet_id, index, &attrs, &raw, &raw_len, 256 * 1024);
        if (err == ESP_ERR_NOT_FOUND) {
            break; /* End of contiguous file table for this applet. */
        }
        if (err != ESP_OK) {
            return err;
        }
        if (raw_len == 0 || neo_import_neo_raw_is_empty(raw, raw_len)) {
            free(raw);
            continue; /* Empty / pad-only Neo slot — not an error. */
        }

        neo_import_saved_file_t *slot = &results[*out_count];
        memset(slot, 0, sizeof(*slot));
        slot->file_index = index;
        strlcpy(slot->name, attrs.name, sizeof(slot->name));

        if (applet_id == NEO_APPLET_ID_ALPHAWORD) {
            size_t text_cap = neo_conv_export_buf_size(raw_len);
            char *text = malloc(text_cap);
            if (!text) {
                free(raw);
                return ESP_ERR_NO_MEM;
            }
            size_t text_len = neo_conv_export_text_from_neo(raw, raw_len, text, text_cap, map);
            free(raw);
            raw = NULL;
            if (text_len == 0 || neo_import_text_is_blank(text, text_len)) {
                free(text);
                continue;
            }
            neo_document_t doc = {
                .file_index = index,
                .file_name = attrs.name,
                .utf8_text = text,
                .utf8_text_length = text_len,
            };
            /* Force write — unlike Backup now / autobackup which skip unchanged. */
            err = neo_import_save_document_force(&doc, slot->path, sizeof(slot->path));
            slot->bytes_saved = text_len;
            free(text);
            if (err == ESP_ERR_NOT_FOUND) {
                continue;
            }
        } else {
            err = neo_import_save_raw_document(raw, raw_len, attrs.name, index, slot->path,
                                             sizeof(slot->path));
            slot->bytes_saved = raw_len;
            free(raw);
            raw = NULL;
        }
        if (err != ESP_OK) {
            return err;
        }
        (*out_count)++;
        ESP_LOGI(TAG, "backed up index=%u name=%s bytes=%u path=%s", index, slot->name,
                 (unsigned)slot->bytes_saved, slot->path);
    }
    return ESP_OK;
}
