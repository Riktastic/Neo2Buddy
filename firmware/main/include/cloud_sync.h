/**
 * @file cloud_sync.h
 * @brief Optional WebDAV / S3-compatible upload of local Neo backups.
 *
 * Local SD/spiflash copies remain authoritative; cloud upload never deletes
 * local files. Credentials live in a dedicated NVS namespace and are never
 * returned over HTTP.
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUD_SYNC_ENDPOINT_MAX 256
#define CLOUD_SYNC_FOLDER_MAX 128
#define CLOUD_SYNC_BUCKET_MAX 64
#define CLOUD_SYNC_REGION_MAX 32
#define CLOUD_SYNC_USERNAME_MAX 64
#define CLOUD_SYNC_SECRET_MAX 128

typedef enum {
    CLOUD_SYNC_PROVIDER_NONE = 0,
    CLOUD_SYNC_PROVIDER_WEBDAV,
    CLOUD_SYNC_PROVIDER_S3,
} cloud_sync_provider_t;

typedef struct {
    cloud_sync_provider_t provider;
    bool enabled;
    char endpoint[CLOUD_SYNC_ENDPOINT_MAX];
    char folder[CLOUD_SYNC_FOLDER_MAX];
    char bucket[CLOUD_SYNC_BUCKET_MAX];
    char region[CLOUD_SYNC_REGION_MAX];
    char username[CLOUD_SYNC_USERNAME_MAX];
    bool credentials_configured;
} cloud_sync_public_config_t;

typedef struct {
    bool busy;
    char phase[16];
    uint8_t current;
    uint8_t total;
    uint32_t uploaded;
    uint32_t failed;
    bool last_ok;
    char last_message[128];
    bool last_test_ok;
    char last_test_message[128];
} cloud_sync_status_t;

void cloud_sync_init(void);

esp_err_t cloud_sync_get_public_config(cloud_sync_public_config_t *out);
esp_err_t cloud_sync_apply_config_json(const cJSON *root, char *err, size_t err_size);
esp_err_t cloud_sync_test(char *message, size_t message_size);
esp_err_t cloud_sync_start_run(void);
void cloud_sync_get_status(cloud_sync_status_t *out);
bool cloud_sync_is_busy(void);

/** Append non-secret fields + status to a JSON object for GET /sync/config. */
esp_err_t cloud_sync_json_config(cJSON *root);
