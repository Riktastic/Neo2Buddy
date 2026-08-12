/**
 * @file cloud_sync.c
 * @brief WebDAV PUT, S3 SigV4, and Hammer Ink sync uploads for local Neo backups.
 */

#include "cloud_sync.h"

#include "file_manager.h"
#include "hammer_ink.h"
#include "log_buffer.h"
#include "wifi_manager.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "cloud_sync";

#define CLOUD_SYNC_NVS_NS "cloud_sync"
#define CLOUD_SYNC_TASK_STACK 16384
#define CLOUD_SYNC_HTTP_TIMEOUT_MS 120000
#define CLOUD_SYNC_HTTP_RETRIES 3
#define CLOUD_SYNC_RETRY_BASE_MS 400
#define CLOUD_SYNC_TEST_BODY "neo2buddy connectivity test\n"

typedef struct {
    cloud_sync_provider_t provider;
    bool enabled;
    char endpoint[CLOUD_SYNC_ENDPOINT_MAX];
    char folder[CLOUD_SYNC_FOLDER_MAX];
    char bucket[CLOUD_SYNC_BUCKET_MAX];
    char region[CLOUD_SYNC_REGION_MAX];
    char username[CLOUD_SYNC_USERNAME_MAX];
    char secret[CLOUD_SYNC_SECRET_MAX];
} cloud_sync_config_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_busy;
static cloud_sync_status_t s_status;

static void cloud_sync_defaults(cloud_sync_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->region, "us-east-1", sizeof(cfg->region));
}

static bool cloud_sync_https_required(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

static void cloud_sync_trim_slashes(char *s)
{
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '/') {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == '/') {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

static void cloud_sync_join_url(char *out, size_t out_size, const char *base, const char *suffix)
{
    char b[CLOUD_SYNC_ENDPOINT_MAX + CLOUD_SYNC_FOLDER_MAX + 64];
    char suf[FILE_MANAGER_NAME_MAX + 64];
    strlcpy(b, base ? base : "", sizeof(b));
    strlcpy(suf, suffix ? suffix : "", sizeof(suf));
    cloud_sync_trim_slashes(b);
    cloud_sync_trim_slashes(suf);
    if (b[0] == '\0') {
        snprintf(out, out_size, "/%s", suf);
        return;
    }
    if (suf[0] == '\0') {
        snprintf(out, out_size, "%s", b);
        return;
    }
    snprintf(out, out_size, "%s/%s", b, suf);
}

static esp_err_t cloud_sync_load(cloud_sync_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_sync_defaults(cfg);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLOUD_SYNC_NVS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t provider = 0;
    uint8_t enabled = 0;
    nvs_get_u8(handle, "provider", &provider);
    nvs_get_u8(handle, "enabled", &enabled);
    cfg->provider = (cloud_sync_provider_t)provider;
    cfg->enabled = enabled != 0;
    size_t len = sizeof(cfg->endpoint);
    nvs_get_str(handle, "endpoint", cfg->endpoint, &len);
    len = sizeof(cfg->folder);
    nvs_get_str(handle, "folder", cfg->folder, &len);
    len = sizeof(cfg->bucket);
    nvs_get_str(handle, "bucket", cfg->bucket, &len);
    len = sizeof(cfg->region);
    nvs_get_str(handle, "region", cfg->region, &len);
    len = sizeof(cfg->username);
    nvs_get_str(handle, "username", cfg->username, &len);
    len = sizeof(cfg->secret);
    nvs_get_str(handle, "secret", cfg->secret, &len);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t cloud_sync_save(const cloud_sync_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLOUD_SYNC_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, "provider", (uint8_t)cfg->provider);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "enabled", cfg->enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "endpoint", cfg->endpoint);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "folder", cfg->folder);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "bucket", cfg->bucket);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "region", cfg->region);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "username", cfg->username);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "secret", cfg->secret);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t cloud_sync_basic_auth(const cloud_sync_config_t *cfg, char *out, size_t out_size)
{
    if (!cfg || out_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    char userpass[CLOUD_SYNC_USERNAME_MAX + CLOUD_SYNC_SECRET_MAX + 2];
    snprintf(userpass, sizeof(userpass), "%s:%s", cfg->username, cfg->secret);
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)out, out_size, &olen, (const unsigned char *)userpass,
                                   strlen(userpass));
    if (rc != 0) {
        return ESP_FAIL;
    }
    out[olen] = '\0';
    return ESP_OK;
}

static void cloud_sync_hex_lower(const uint8_t *in, size_t len, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static esp_err_t cloud_sync_sha256_hex(const uint8_t *data, size_t len, char *out_hex)
{
    uint8_t digest[32];
    int rc = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, len, digest);
    if (rc != 0) {
        return ESP_FAIL;
    }
    cloud_sync_hex_lower(digest, sizeof(digest), out_hex);
    return ESP_OK;
}

static esp_err_t cloud_sync_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                                        uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return ESP_FAIL;
    }
    int rc = mbedtls_md_hmac(info, key, key_len, data, data_len, out);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static void cloud_sync_uri_encode_path(const char *path, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; path[i] != '\0' && o + 4 < out_size; i++) {
        unsigned char c = (unsigned char)path[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, out_size - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

static esp_err_t cloud_sync_time_valid(void)
{
    time_t now = time(NULL);
    return now >= (time_t)1609459200 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void cloud_sync_persist_runtime_status(void)
{
    nvs_handle_t handle;
    if (nvs_open(CLOUD_SYNC_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u8(handle, "last_ok", s_status.last_ok ? 1 : 0);
    nvs_set_str(handle, "last_msg", s_status.last_message);
    nvs_set_u8(handle, "test_ok", s_status.last_test_ok ? 1 : 0);
    nvs_set_str(handle, "test_msg", s_status.last_test_message);
    nvs_commit(handle);
    nvs_close(handle);
}

static void cloud_sync_restore_runtime_status(void)
{
    nvs_handle_t handle;
    if (nvs_open(CLOUD_SYNC_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t value = 0;
    if (nvs_get_u8(handle, "last_ok", &value) == ESP_OK) {
        s_status.last_ok = value != 0;
    }
    size_t len = sizeof(s_status.last_message);
    nvs_get_str(handle, "last_msg", s_status.last_message, &len);
    if (nvs_get_u8(handle, "test_ok", &value) == ESP_OK) {
        s_status.last_test_ok = value != 0;
    }
    len = sizeof(s_status.last_test_message);
    nvs_get_str(handle, "test_msg", s_status.last_test_message, &len);
    nvs_close(handle);
}

static bool cloud_sync_config_complete(const cloud_sync_config_t *cfg)
{
    return cfg && cfg->provider != CLOUD_SYNC_PROVIDER_NONE && cfg->endpoint[0] != '\0' &&
           cfg->username[0] != '\0' && cfg->secret[0] != '\0' &&
           (cfg->provider != CLOUD_SYNC_PROVIDER_S3 || cfg->bucket[0] != '\0');
}

static bool cloud_sync_config_ready(const cloud_sync_config_t *cfg)
{
    return cloud_sync_config_complete(cfg) && cfg->enabled;
}

static void cloud_sync_format_http_error(int status, char *err, size_t err_size)
{
    switch (status) {
    case 401:
        snprintf(err, err_size, "Unauthorized — check username and password/key");
        break;
    case 403:
        snprintf(err, err_size, "Forbidden — check folder or bucket permissions");
        break;
    case 404:
        snprintf(err, err_size, "Not found — check server URL, bucket, or folder path");
        break;
    case 405:
        snprintf(err, err_size, "Method not allowed");
        break;
    case 409:
        snprintf(err, err_size, "Conflict — path exists but is not writable");
        break;
    case 412:
        snprintf(err, err_size, "Precondition failed — parent folder may be missing");
        break;
    case 423:
        snprintf(err, err_size, "Locked — file or folder is locked on the server");
        break;
    case 507:
        snprintf(err, err_size, "Insufficient storage on the server");
        break;
    default:
        if (status >= 500) {
            snprintf(err, err_size, "Server error HTTP %d", status);
        } else if (status > 0) {
            snprintf(err, err_size, "HTTP %d", status);
        } else {
            snprintf(err, err_size, "No HTTP response");
        }
        break;
    }
}

static bool cloud_sync_http_retryable(esp_err_t err, int status)
{
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_FETCH_HEADER) {
        return true;
    }
    if (err != ESP_OK && status <= 0) {
        return true;
    }
    return status == 408 || status == 429 || status >= 500;
}

static esp_err_t cloud_sync_s3_sign(const cloud_sync_config_t *cfg, const char *method, const char *host,
                                    const char *canonical_uri, const char *payload_hash, const char *amz_date,
                                    const char *date_stamp, char *authorization, size_t authorization_size)
{
    char canonical_headers[384];
    char signed_headers[] = "host;x-amz-content-sha256;x-amz-date";
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\n", host, payload_hash, amz_date);

    char canonical_request[768];
    snprintf(canonical_request, sizeof(canonical_request), "%s\n%s\n\n%s\n%s\n%s", method, canonical_uri,
             canonical_headers, signed_headers, payload_hash);

    char canonical_hash[65];
    if (cloud_sync_sha256_hex((const uint8_t *)canonical_request, strlen(canonical_request), canonical_hash) !=
        ESP_OK) {
        return ESP_FAIL;
    }

    const char *region = cfg->region[0] ? cfg->region : "us-east-1";
    char credential_scope[96];
    snprintf(credential_scope, sizeof(credential_scope), "%s/%s/s3/aws4_request", date_stamp, region);

    char string_to_sign[512];
    snprintf(string_to_sign, sizeof(string_to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, credential_scope,
             canonical_hash);

    uint8_t k_date[32];
    uint8_t k_region[32];
    uint8_t k_service[32];
    uint8_t k_signing[32];
    uint8_t signature[32];

    char secret_key[CLOUD_SYNC_SECRET_MAX + 8];
    snprintf(secret_key, sizeof(secret_key), "AWS4%s", cfg->secret);
    if (cloud_sync_hmac_sha256((const uint8_t *)secret_key, strlen(secret_key), (const uint8_t *)date_stamp,
                               strlen(date_stamp), k_date) != ESP_OK) {
        return ESP_FAIL;
    }
    if (cloud_sync_hmac_sha256(k_date, sizeof(k_date), (const uint8_t *)region, strlen(region), k_region) !=
        ESP_OK) {
        return ESP_FAIL;
    }
    if (cloud_sync_hmac_sha256(k_region, sizeof(k_region), (const uint8_t *)"s3", 2, k_service) != ESP_OK) {
        return ESP_FAIL;
    }
    if (cloud_sync_hmac_sha256(k_service, sizeof(k_service), (const uint8_t *)"aws4_request", 12, k_signing) !=
        ESP_OK) {
        return ESP_FAIL;
    }
    if (cloud_sync_hmac_sha256(k_signing, sizeof(k_signing), (const uint8_t *)string_to_sign, strlen(string_to_sign),
                               signature) != ESP_OK) {
        return ESP_FAIL;
    }

    char signature_hex[65];
    cloud_sync_hex_lower(signature, sizeof(signature), signature_hex);
    snprintf(authorization, authorization_size,
             "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s", cfg->username, credential_scope,
             signed_headers, signature_hex);
    return ESP_OK;
}

static esp_err_t cloud_sync_http_request_once(const cloud_sync_config_t *cfg, esp_http_client_method_t method,
                                              const char *url, const char *host, const char *canonical_uri,
                                              const uint8_t *body, size_t body_len, bool s3, int *out_status,
                                              char *err_msg, size_t err_size)
{
    if (!cloud_sync_https_required(url)) {
        snprintf(err_msg, err_size, "HTTPS required");
        if (out_status) {
            *out_status = 0;
        }
        return ESP_ERR_INVALID_ARG;
    }

    char payload_hash[65];
    if (body_len > 0) {
        if (cloud_sync_sha256_hex(body, body_len, payload_hash) != ESP_OK) {
            snprintf(err_msg, err_size, "hash failed");
            if (out_status) {
                *out_status = 0;
            }
            return ESP_FAIL;
        }
    } else {
        strlcpy(payload_hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                sizeof(payload_hash));
    }

    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) == NULL) {
        snprintf(err_msg, err_size, "clock unavailable");
        if (out_status) {
            *out_status = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }
    char amz_date[32];
    char date_stamp[16];
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm_utc);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", &tm_utc);

    char auth_basic[192];
    char auth_s3[512];
    const char *auth_header = NULL;
    if (s3) {
        if (cloud_sync_s3_sign(cfg, method == HTTP_METHOD_PUT ? "PUT" : "MKCOL", host, canonical_uri, payload_hash,
                               amz_date, date_stamp, auth_s3, sizeof(auth_s3)) != ESP_OK) {
            snprintf(err_msg, err_size, "S3 sign failed");
            if (out_status) {
                *out_status = 0;
            }
            return ESP_FAIL;
        }
        auth_header = auth_s3;
    } else if (cfg->username[0] != '\0') {
        if (cloud_sync_basic_auth(cfg, auth_basic, sizeof(auth_basic)) != ESP_OK) {
            snprintf(err_msg, err_size, "auth encode failed");
            if (out_status) {
                *out_status = 0;
            }
            return ESP_FAIL;
        }
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = method,
        .timeout_ms = CLOUD_SYNC_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        snprintf(err_msg, err_size, "http init failed");
        if (out_status) {
            *out_status = 0;
        }
        return ESP_ERR_NO_MEM;
    }

    if (s3) {
        esp_http_client_set_header(client, "Authorization", auth_header);
        esp_http_client_set_header(client, "x-amz-content-sha256", payload_hash);
        esp_http_client_set_header(client, "x-amz-date", amz_date);
    } else if (cfg->username[0] != '\0') {
        char basic_hdr[256];
        snprintf(basic_hdr, sizeof(basic_hdr), "Basic %s", auth_basic);
        esp_http_client_set_header(client, "Authorization", basic_hdr);
    }
    if (method == HTTP_METHOD_PUT) {
        esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
    }

    esp_err_t ret = esp_http_client_open(client, (int)body_len);
    if (ret != ESP_OK) {
        snprintf(err_msg, err_size, "connection failed");
        if (out_status) {
            *out_status = 0;
        }
        esp_http_client_cleanup(client);
        return ret;
    }
    if (body_len > 0) {
        int written = esp_http_client_write(client, (const char *)body, (int)body_len);
        if (written < 0 || (size_t)written != body_len) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(err_msg, err_size, "write failed");
            if (out_status) {
                *out_status = 0;
            }
            return ESP_FAIL;
        }
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (out_status) {
        *out_status = status;
    }
    if (status >= 200 && status < 300) {
        return ESP_OK;
    }
    cloud_sync_format_http_error(status, err_msg, err_size);
    return ESP_FAIL;
}

static esp_err_t cloud_sync_http_request(const cloud_sync_config_t *cfg, esp_http_client_method_t method,
                                         const char *url, const char *host, const char *canonical_uri,
                                         const uint8_t *body, size_t body_len, bool s3, char *err_msg,
                                         size_t err_size)
{
    esp_err_t last_err = ESP_FAIL;
    int last_status = 0;
    for (int attempt = 0; attempt < CLOUD_SYNC_HTTP_RETRIES; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(CLOUD_SYNC_RETRY_BASE_MS * (uint32_t)attempt));
        }
        last_err = cloud_sync_http_request_once(cfg, method, url, host, canonical_uri, body, body_len, s3,
                                                &last_status, err_msg, err_size);
        if (last_err == ESP_OK) {
            return ESP_OK;
        }
        if (!cloud_sync_http_retryable(last_err, last_status)) {
            break;
        }
        ESP_LOGW(TAG, "HTTP %s retry %d for %s (%s)", method == HTTP_METHOD_PUT ? "PUT" : "MKCOL", attempt + 1, url,
                 err_msg);
    }
    if (last_status > 0 && err_msg[0] == '\0') {
        cloud_sync_format_http_error(last_status, err_msg, err_size);
    }
    return last_err;
}

static esp_err_t cloud_sync_webdav_mkcol(const cloud_sync_config_t *cfg, const char *collection_url, char *err,
                                         size_t err_size)
{
    const char *host_start = strstr(collection_url, "://");
    host_start = host_start ? host_start + 3 : collection_url;
    const char *path_start = strchr(host_start, '/');
    char host[128];
    char canonical_uri[384];
    size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
    if (host_len >= sizeof(host)) {
        snprintf(err, err_size, "host too long");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    if (path_start) {
        strlcpy(canonical_uri, path_start, sizeof(canonical_uri));
    } else {
        canonical_uri[0] = '/';
        canonical_uri[1] = '\0';
    }

    int status = 0;
    esp_err_t ret = cloud_sync_http_request_once(cfg, HTTP_METHOD_MKCOL, collection_url, host, canonical_uri, NULL, 0,
                                                 false, &status, err, err_size);
    if (ret == ESP_OK || status == 405 || status == 409) {
        return ESP_OK;
    }
    if (status == 401 || status == 403 || status == 404) {
        return ret;
    }
    if (status == 301 || status == 302 || status == 307 || status == 308) {
        snprintf(err, err_size, "Redirect not supported — use the final HTTPS URL");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ret;
}

static esp_err_t cloud_sync_webdav_ensure_folders(const cloud_sync_config_t *cfg, char *err, size_t err_size)
{
    if (!cfg || cfg->provider != CLOUD_SYNC_PROVIDER_WEBDAV || cfg->folder[0] == '\0') {
        return ESP_OK;
    }

    char segment_path[CLOUD_SYNC_FOLDER_MAX + 8];
    strlcpy(segment_path, cfg->folder, sizeof(segment_path));
    cloud_sync_trim_slashes(segment_path);
    if (segment_path[0] == '\0') {
        return ESP_OK;
    }

    char current[CLOUD_SYNC_ENDPOINT_MAX + CLOUD_SYNC_FOLDER_MAX + 16];
    strlcpy(current, cfg->endpoint, sizeof(current));
    cloud_sync_trim_slashes(current);

    char *saveptr = NULL;
    for (char *segment = strtok_r(segment_path, "/", &saveptr); segment != NULL;
         segment = strtok_r(NULL, "/", &saveptr)) {
        char collection_url[512];
        cloud_sync_join_url(collection_url, sizeof(collection_url), current, segment);
        esp_err_t mk = cloud_sync_webdav_mkcol(cfg, collection_url, err, err_size);
        if (mk != ESP_OK) {
            return mk;
        }
        strlcpy(current, collection_url, sizeof(current));
    }
    return ESP_OK;
}

static esp_err_t cloud_sync_http_put_buffer(const cloud_sync_config_t *cfg, const char *url, const char *host,
                                            const char *canonical_uri, const uint8_t *body, size_t body_len, bool s3,
                                            char *err_msg, size_t err_size)
{
    if (cfg->provider == CLOUD_SYNC_PROVIDER_WEBDAV) {
        esp_err_t folders = cloud_sync_webdav_ensure_folders(cfg, err_msg, err_size);
        if (folders != ESP_OK) {
            return folders;
        }
    }
    return cloud_sync_http_request(cfg, HTTP_METHOD_PUT, url, host, canonical_uri, body, body_len, s3, err_msg,
                                   err_size);
}

static esp_err_t cloud_sync_build_target_url(const cloud_sync_config_t *cfg, const char *filename, char *url,
                                             size_t url_size, char *host_out, size_t host_size,
                                             char *canonical_uri, size_t canonical_uri_size, bool *out_s3)
{
    if (!cfg || !filename || !url || !out_s3) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_s3 = cfg->provider == CLOUD_SYNC_PROVIDER_S3;

    char object_key[FILE_MANAGER_NAME_MAX + CLOUD_SYNC_FOLDER_MAX + 8];
    if (cfg->folder[0] != '\0') {
        snprintf(object_key, sizeof(object_key), "%s/%s", cfg->folder, filename);
    } else {
        strlcpy(object_key, filename, sizeof(object_key));
    }
    cloud_sync_trim_slashes(object_key);

    if (cfg->provider == CLOUD_SYNC_PROVIDER_WEBDAV) {
        cloud_sync_join_url(url, url_size, cfg->endpoint, object_key);
        const char *host_start = strstr(cfg->endpoint, "://");
        host_start = host_start ? host_start + 3 : cfg->endpoint;
        const char *path_start = strchr(host_start, '/');
        size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
        if (host_len >= host_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(host_out, host_start, host_len);
        host_out[host_len] = '\0';
        snprintf(canonical_uri, canonical_uri_size, "/%s", object_key);
        return ESP_OK;
    }

    if (cfg->provider == CLOUD_SYNC_PROVIDER_S3) {
        char encoded_key[CLOUD_SYNC_FOLDER_MAX + FILE_MANAGER_NAME_MAX + 16];
        cloud_sync_uri_encode_path(object_key, encoded_key, sizeof(encoded_key));
        if (cfg->bucket[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        cloud_sync_join_url(url, url_size, cfg->endpoint, "");
        size_t base_len = strlen(url);
        snprintf(url + base_len, url_size - base_len, "/%s/%s", cfg->bucket, encoded_key);
        const char *host_start = strstr(cfg->endpoint, "://");
        host_start = host_start ? host_start + 3 : cfg->endpoint;
        const char *path_start = strchr(host_start, '/');
        size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
        if (host_len >= host_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(host_out, host_start, host_len);
        host_out[host_len] = '\0';
        snprintf(canonical_uri, canonical_uri_size, "/%s/%s", cfg->bucket, encoded_key);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t cloud_sync_upload_file(const cloud_sync_config_t *cfg, const char *filename, char *err,
                                        size_t err_size)
{
    char path[320];
    if (file_manager_resolve_path(filename, path, sizeof(path)) != ESP_OK) {
        snprintf(err, err_size, "bad name");
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "open local file failed");
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, err_size, "seek failed");
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)FILE_MANAGER_MAX_UPLOAD) {
        fclose(f);
        snprintf(err, err_size, "file too large");
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, err_size, "seek failed");
        return ESP_FAIL;
    }
    uint8_t *body = NULL;
    if (sz > 0) {
        body = malloc((size_t)sz);
        if (!body) {
            fclose(f);
            snprintf(err, err_size, "no memory");
            return ESP_ERR_NO_MEM;
        }
        if (fread(body, 1, (size_t)sz, f) != (size_t)sz) {
            free(body);
            fclose(f);
            snprintf(err, err_size, "read failed");
            return ESP_FAIL;
        }
    }
    fclose(f);

    char url[512];
    char host[128];
    char canonical_uri[384];
    bool s3 = false;
    esp_err_t build = cloud_sync_build_target_url(cfg, filename, url, sizeof(url), host, sizeof(host), canonical_uri,
                                                  sizeof(canonical_uri), &s3);
    if (build != ESP_OK) {
        free(body);
        snprintf(err, err_size, "bad destination");
        return build;
    }

    esp_err_t up = cloud_sync_http_put_buffer(cfg, url, host, canonical_uri, body, (size_t)sz, s3, err, err_size);
    free(body);
    return up;
}

static int cloud_sync_count_backup_files(void)
{
    if (file_manager_ensure_dir() != ESP_OK) {
        return 0;
    }
    const char *base = file_manager_base_path();
    DIR *dir = opendir(base);
    if (!dir) {
        return 0;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen > 4 && strcasecmp(ent->d_name + nlen - 4, ".txt") == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static void cloud_sync_hammer_progress(uint8_t current, uint8_t total, const char *filename, void *ctx)
{
    (void)filename;
    (void)ctx;
    s_status.current = current;
    s_status.total = total;
}

static void cloud_sync_run_task(void *arg)
{
    (void)arg;
    cloud_sync_config_t cfg;
    if (cloud_sync_load(&cfg) != ESP_OK || !cfg.enabled || cfg.provider == CLOUD_SYNC_PROVIDER_NONE ||
        cfg.secret[0] == '\0' || cfg.endpoint[0] == '\0') {
        strlcpy(s_status.phase, "done", sizeof(s_status.phase));
        s_status.last_ok = false;
        strlcpy(s_status.last_message, "Cloud sync not configured", sizeof(s_status.last_message));
        cloud_sync_persist_runtime_status();
        s_busy = false;
        s_status.busy = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (!wifi_manager_is_connected()) {
        strlcpy(s_status.phase, "done", sizeof(s_status.phase));
        s_status.last_ok = false;
        strlcpy(s_status.last_message, "No internet — connect the buddy to home Wi‑Fi first", sizeof(s_status.last_message));
        cloud_sync_persist_runtime_status();
        log_buffer_append_level(LOG_LEVEL_WARN, "cloud: upload skipped (no Wi‑Fi)");
        s_busy = false;
        s_status.busy = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (cfg.provider == CLOUD_SYNC_PROVIDER_S3 && cloud_sync_time_valid() != ESP_OK) {
        strlcpy(s_status.phase, "done", sizeof(s_status.phase));
        s_status.last_ok = false;
        strlcpy(s_status.last_message, "Clock not set — connect to Wi‑Fi for SNTP first", sizeof(s_status.last_message));
        cloud_sync_persist_runtime_status();
        log_buffer_append_level(LOG_LEVEL_WARN, "cloud: upload skipped (clock not set)");
        s_busy = false;
        s_status.busy = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (cfg.provider == CLOUD_SYNC_PROVIDER_HAMMER) {
        strlcpy(s_status.phase, "running", sizeof(s_status.phase));
        s_status.uploaded = 0;
        s_status.failed = 0;
        s_status.current = 0;
        s_status.total = (uint8_t)cloud_sync_count_backup_files();
        log_buffer_appendf("cloud: Hammer upload started (%u file(s))", (unsigned)s_status.total);

        hammer_ink_config_t hcfg = {
            .endpoint = cfg.endpoint,
            .email = cfg.username,
            .password = cfg.secret,
            .project_name = cfg.folder[0] ? cfg.folder : HAMMER_INK_DEFAULT_PROJECT,
        };
        uint32_t up = 0;
        uint32_t fail = 0;
        char err[128];
        esp_err_t hret =
            hammer_ink_upload_backups(&hcfg, &up, &fail, cloud_sync_hammer_progress, NULL, err, sizeof(err));
        s_status.uploaded = up;
        s_status.failed = fail;
        s_status.last_ok = (hret == ESP_OK && fail == 0);
        strlcpy(s_status.last_message, err, sizeof(s_status.last_message));
        if (s_status.last_ok) {
            log_buffer_appendf("cloud: Hammer upload finished (%u note(s))", (unsigned)up);
        } else {
            log_buffer_append_level(LOG_LEVEL_WARN, "cloud: Hammer upload finished with errors: %s", err);
        }
        strlcpy(s_status.phase, "done", sizeof(s_status.phase));
        cloud_sync_persist_runtime_status();
        s_busy = false;
        s_status.busy = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (cfg.provider == CLOUD_SYNC_PROVIDER_WEBDAV) {
        char folder_err[96];
        if (cloud_sync_webdav_ensure_folders(&cfg, folder_err, sizeof(folder_err)) != ESP_OK) {
            strlcpy(s_status.phase, "done", sizeof(s_status.phase));
            s_status.last_ok = false;
            snprintf(s_status.last_message, sizeof(s_status.last_message), "Folder setup failed: %.80s", folder_err);
            cloud_sync_persist_runtime_status();
            log_buffer_append_level(LOG_LEVEL_ERROR, "cloud: folder setup failed: %s", folder_err);
            s_busy = false;
            s_status.busy = false;
            s_task = NULL;
            vTaskDelete(NULL);
            return;
        }
    }

    strlcpy(s_status.phase, "running", sizeof(s_status.phase));
    s_status.uploaded = 0;
    s_status.failed = 0;
    s_status.current = 0;
    s_status.total = (uint8_t)cloud_sync_count_backup_files();
    log_buffer_appendf("cloud: upload started (%u file(s))", (unsigned)s_status.total);

    const char *base = file_manager_base_path();
    DIR *dir = opendir(base);
    if (!dir) {
        s_status.last_ok = false;
        strlcpy(s_status.last_message, "Cannot open backup folder", sizeof(s_status.last_message));
        strlcpy(s_status.phase, "done", sizeof(s_status.phase));
        cloud_sync_persist_runtime_status();
        log_buffer_append_level(LOG_LEVEL_ERROR, "cloud: cannot open backup folder");
        s_busy = false;
        s_status.busy = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 4 || strcasecmp(ent->d_name + nlen - 4, ".txt") != 0) {
            continue;
        }
        if (file_manager_validate_name(ent->d_name) != ESP_OK) {
            continue;
        }
        s_status.current++;
        char err[96];
        if (cloud_sync_upload_file(&cfg, ent->d_name, err, sizeof(err)) == ESP_OK) {
            s_status.uploaded++;
        } else {
            s_status.failed++;
            snprintf(s_status.last_message, sizeof(s_status.last_message), "%.48s: %.64s", ent->d_name, err);
            ESP_LOGW(TAG, "upload %s failed: %s", ent->d_name, err);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    closedir(dir);

    s_status.last_ok = s_status.failed == 0;
    if (s_status.last_ok) {
        snprintf(s_status.last_message, sizeof(s_status.last_message), "Uploaded %u file(s)",
                 (unsigned)s_status.uploaded);
        log_buffer_appendf("cloud: upload finished (%u file(s))", (unsigned)s_status.uploaded);
    } else {
        snprintf(s_status.last_message, sizeof(s_status.last_message), "%u uploaded, %u failed",
                 (unsigned)s_status.uploaded, (unsigned)s_status.failed);
        log_buffer_append_level(LOG_LEVEL_WARN, "cloud: upload finished with %u failure(s)", (unsigned)s_status.failed);
    }
    strlcpy(s_status.phase, "done", sizeof(s_status.phase));
    cloud_sync_persist_runtime_status();
    s_busy = false;
    s_status.busy = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

void cloud_sync_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.phase, "idle", sizeof(s_status.phase));
    cloud_sync_restore_runtime_status();
}

esp_err_t cloud_sync_get_public_config(cloud_sync_public_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_sync_config_t cfg;
    esp_err_t err = cloud_sync_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->provider = cfg.provider;
    out->enabled = cfg.enabled;
    strlcpy(out->endpoint, cfg.endpoint, sizeof(out->endpoint));
    strlcpy(out->folder, cfg.folder, sizeof(out->folder));
    strlcpy(out->bucket, cfg.bucket, sizeof(out->bucket));
    strlcpy(out->region, cfg.region, sizeof(out->region));
    strlcpy(out->username, cfg.username, sizeof(out->username));
    out->credentials_configured = cfg.secret[0] != '\0';
    return ESP_OK;
}

static const char *cloud_sync_provider_string(cloud_sync_provider_t p)
{
    switch (p) {
    case CLOUD_SYNC_PROVIDER_WEBDAV:
        return "webdav";
    case CLOUD_SYNC_PROVIDER_S3:
        return "s3";
    case CLOUD_SYNC_PROVIDER_HAMMER:
        return "hammer";
    default:
        return "none";
    }
}

static cloud_sync_provider_t cloud_sync_provider_from_string(const char *s)
{
    if (!s) {
        return CLOUD_SYNC_PROVIDER_NONE;
    }
    if (strcmp(s, "webdav") == 0) {
        return CLOUD_SYNC_PROVIDER_WEBDAV;
    }
    if (strcmp(s, "s3") == 0) {
        return CLOUD_SYNC_PROVIDER_S3;
    }
    if (strcmp(s, "hammer") == 0) {
        return CLOUD_SYNC_PROVIDER_HAMMER;
    }
    return CLOUD_SYNC_PROVIDER_NONE;
}

esp_err_t cloud_sync_apply_config_json(const cJSON *root, char *err, size_t err_size)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_sync_config_t cfg;
    cloud_sync_load(&cfg);

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "provider");
    if (cJSON_IsString(item) && item->valuestring) {
        cfg.provider = cloud_sync_provider_from_string(item->valuestring);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(item)) {
        cfg.enabled = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "endpoint");
    if (cJSON_IsString(item) && item->valuestring) {
        if (strlen(item->valuestring) >= sizeof(cfg.endpoint)) {
            snprintf(err, err_size, "endpoint too long");
            return ESP_ERR_INVALID_SIZE;
        }
        if (item->valuestring[0] != '\0' && !cloud_sync_https_required(item->valuestring)) {
            snprintf(err, err_size, "endpoint must use https://");
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.endpoint, item->valuestring, sizeof(cfg.endpoint));
    }
    if (cfg.provider == CLOUD_SYNC_PROVIDER_HAMMER && cfg.endpoint[0] == '\0') {
        strlcpy(cfg.endpoint, HAMMER_INK_DEFAULT_ENDPOINT, sizeof(cfg.endpoint));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "folder");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(cfg.folder, item->valuestring, sizeof(cfg.folder));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(cfg.folder, item->valuestring, sizeof(cfg.folder));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "bucket");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(cfg.bucket, item->valuestring, sizeof(cfg.bucket));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "region");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(cfg.region, item->valuestring, sizeof(cfg.region));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "username");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(cfg.username, item->valuestring, sizeof(cfg.username));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "secret");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        if (strlen(item->valuestring) >= sizeof(cfg.secret)) {
            snprintf(err, err_size, "secret too long");
            return ESP_ERR_INVALID_SIZE;
        }
        strlcpy(cfg.secret, item->valuestring, sizeof(cfg.secret));
    }

    if (cfg.enabled && cfg.provider != CLOUD_SYNC_PROVIDER_NONE) {
        if (cfg.endpoint[0] == '\0') {
            snprintf(err, err_size, "endpoint required");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg.username[0] == '\0') {
            snprintf(err, err_size,
                     cfg.provider == CLOUD_SYNC_PROVIDER_HAMMER ? "email required" : "username or access key required");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg.secret[0] == '\0') {
            snprintf(err, err_size,
                     cfg.provider == CLOUD_SYNC_PROVIDER_HAMMER ? "password required"
                                                                : "app password or secret key required");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg.provider == CLOUD_SYNC_PROVIDER_S3 && cfg.bucket[0] == '\0') {
            snprintf(err, err_size, "bucket required for S3");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg.provider == CLOUD_SYNC_PROVIDER_HAMMER && cfg.folder[0] == '\0') {
            strlcpy(cfg.folder, HAMMER_INK_DEFAULT_PROJECT, sizeof(cfg.folder));
        }
    }

    esp_err_t save_err = cloud_sync_save(&cfg);
    if (save_err == ESP_OK) {
        log_buffer_appendf("cloud: settings saved (%s)", cloud_sync_provider_string(cfg.provider));
    }
    return save_err;
}

static void cloud_sync_json_health(cJSON *health, const cloud_sync_config_t *cfg)
{
    if (!health || !cfg) {
        return;
    }

    const bool configured = cloud_sync_config_complete(cfg);
    const bool wifi_ok = wifi_manager_is_connected();
    const bool clock_ok = cloud_sync_time_valid() == ESP_OK;
    const bool needs_clock = cfg->provider == CLOUD_SYNC_PROVIDER_S3;
    const bool ready = cloud_sync_config_ready(cfg) && wifi_ok && (!needs_clock || clock_ok);

    cJSON_AddBoolToObject(health, "configured", configured);
    cJSON_AddBoolToObject(health, "enabled", cfg->enabled);
    cJSON_AddBoolToObject(health, "wifi_ok", wifi_ok);
    cJSON_AddBoolToObject(health, "clock_ok", clock_ok);
    cJSON_AddBoolToObject(health, "ready", ready);

    const char *state = "idle";
    if (cfg->enabled && configured) {
        if (!wifi_ok || (needs_clock && !clock_ok)) {
            state = "warning";
        } else if (!s_status.last_test_ok && s_status.last_test_message[0] != '\0') {
            state = "warning";
        } else if (!s_status.last_ok && s_status.last_message[0] != '\0' && strcmp(s_status.phase, "idle") != 0) {
            state = "error";
        } else if (s_status.last_test_ok) {
            state = "ok";
        } else {
            state = "warning";
        }
    }
    cJSON_AddStringToObject(health, "state", state);

    cJSON *issues = cJSON_CreateArray();
    if (issues) {
        if (cfg->enabled && !configured) {
            cJSON_AddItemToArray(issues, cJSON_CreateString("Finish endpoint, credentials, and bucket settings"));
        }
        if (cfg->enabled && !wifi_ok) {
            cJSON_AddItemToArray(issues, cJSON_CreateString("Connect the buddy to home Wi‑Fi with internet access"));
        }
        if (cfg->enabled && needs_clock && !clock_ok) {
            cJSON_AddItemToArray(issues, cJSON_CreateString("Wait for the device clock to sync after joining Wi‑Fi"));
        }
        if (s_status.last_test_message[0] != '\0' && !s_status.last_test_ok) {
            cJSON_AddItemToArray(issues, cJSON_CreateString(s_status.last_test_message));
        }
        if (!s_status.busy && !s_status.last_ok && s_status.last_message[0] != '\0' &&
            strcmp(s_status.phase, "idle") != 0) {
            cJSON_AddItemToArray(issues, cJSON_CreateString(s_status.last_message));
        }
        cJSON_AddItemToObject(health, "issues", issues);
    }
}

esp_err_t cloud_sync_test(char *message, size_t message_size)
{
    if (!message || message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_sync_config_t cfg;
    if (cloud_sync_load(&cfg) != ESP_OK) {
        snprintf(message, message_size, "load failed");
        return ESP_FAIL;
    }
    if (cfg.provider == CLOUD_SYNC_PROVIDER_NONE || cfg.secret[0] == '\0' || cfg.endpoint[0] == '\0') {
        snprintf(message, message_size, "Configure provider, endpoint, and credentials first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_is_connected()) {
        snprintf(message, message_size, "No internet — connect the buddy to home Wi‑Fi first");
        s_status.last_test_ok = false;
        strlcpy(s_status.last_test_message, message, sizeof(s_status.last_test_message));
        cloud_sync_persist_runtime_status();
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg.provider == CLOUD_SYNC_PROVIDER_S3 && cloud_sync_time_valid() != ESP_OK) {
        snprintf(message, message_size, "Set clock via Wi‑Fi (SNTP) before S3");
        s_status.last_test_ok = false;
        strlcpy(s_status.last_test_message, message, sizeof(s_status.last_test_message));
        cloud_sync_persist_runtime_status();
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg.provider == CLOUD_SYNC_PROVIDER_S3 && cfg.bucket[0] == '\0') {
        snprintf(message, message_size, "Bucket required for S3");
        return ESP_ERR_INVALID_STATE;
    }

    if (cfg.provider == CLOUD_SYNC_PROVIDER_HAMMER) {
        hammer_ink_config_t hcfg = {
            .endpoint = cfg.endpoint,
            .email = cfg.username,
            .password = cfg.secret,
            .project_name = cfg.folder[0] ? cfg.folder : HAMMER_INK_DEFAULT_PROJECT,
        };
        esp_err_t ht = hammer_ink_test(&hcfg, message, message_size);
        s_status.last_test_ok = (ht == ESP_OK);
        strlcpy(s_status.last_test_message, message, sizeof(s_status.last_test_message));
        cloud_sync_persist_runtime_status();
        if (ht == ESP_OK) {
            log_buffer_appendf("cloud: Hammer connection test OK");
        } else {
            log_buffer_append_level(LOG_LEVEL_WARN, "cloud: Hammer connection test failed: %s", message);
        }
        return ht;
    }

    const uint8_t *body = (const uint8_t *)CLOUD_SYNC_TEST_BODY;
    size_t body_len = strlen(CLOUD_SYNC_TEST_BODY);
    char url[512];
    char host[128];
    char canonical_uri[384];
    bool s3 = false;
    esp_err_t build = cloud_sync_build_target_url(&cfg, ".neo2buddy-test.txt", url, sizeof(url), host, sizeof(host),
                                                  canonical_uri, sizeof(canonical_uri), &s3);
    if (build != ESP_OK) {
        snprintf(message, message_size, "bad destination URL");
        return build;
    }

    char err[96];
    esp_err_t up = cloud_sync_http_put_buffer(&cfg, url, host, canonical_uri, body, body_len, s3, err, sizeof(err));
    if (up == ESP_OK) {
        snprintf(message, message_size, "Test upload OK");
        s_status.last_test_ok = true;
        strlcpy(s_status.last_test_message, message, sizeof(s_status.last_test_message));
        cloud_sync_persist_runtime_status();
        log_buffer_appendf("cloud: connection test OK");
        return ESP_OK;
    }
    snprintf(message, message_size, "Test failed: %s", err);
    s_status.last_test_ok = false;
    strlcpy(s_status.last_test_message, message, sizeof(s_status.last_test_message));
    cloud_sync_persist_runtime_status();
    log_buffer_append_level(LOG_LEVEL_WARN, "cloud: connection test failed: %s", err);
    return up;
}

esp_err_t cloud_sync_start_run(void)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    s_status.busy = true;
    s_status.uploaded = 0;
    s_status.failed = 0;
    s_status.current = 0;
    s_status.total = 0;
    strlcpy(s_status.phase, "starting", sizeof(s_status.phase));
    s_status.last_message[0] = '\0';
    xSemaphoreGive(s_lock);

    BaseType_t ok = xTaskCreate(cloud_sync_run_task, "cloud_sync", CLOUD_SYNC_TASK_STACK, NULL, 4, &s_task);
    if (ok != pdPASS) {
        s_busy = false;
        s_status.busy = false;
        strlcpy(s_status.phase, "idle", sizeof(s_status.phase));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cloud_sync_get_status(cloud_sync_status_t *out)
{
    if (!out) {
        return;
    }
    *out = s_status;
    out->busy = s_busy;
}

bool cloud_sync_is_busy(void)
{
    return s_busy;
}

esp_err_t cloud_sync_json_config(cJSON *root)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_sync_public_config_t pub;
    esp_err_t err = cloud_sync_get_public_config(&pub);
    if (err != ESP_OK) {
        return err;
    }
    cJSON_AddStringToObject(root, "provider", cloud_sync_provider_string(pub.provider));
    cJSON_AddBoolToObject(root, "enabled", pub.enabled);
    cJSON_AddStringToObject(root, "endpoint", pub.endpoint);
    cJSON_AddStringToObject(root, "folder", pub.folder);
    cJSON_AddStringToObject(root, "path", pub.folder);
    cJSON_AddStringToObject(root, "bucket", pub.bucket);
    cJSON_AddStringToObject(root, "region", pub.region);
    cJSON_AddStringToObject(root, "username", pub.username);
    cJSON_AddBoolToObject(root, "credentials_configured", pub.credentials_configured);

    cloud_sync_status_t st;
    cloud_sync_get_status(&st);
    cJSON *status = cJSON_CreateObject();
    if (!status) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(status, "busy", st.busy);
    cJSON_AddStringToObject(status, "phase", st.phase);
    cJSON_AddNumberToObject(status, "current", st.current);
    cJSON_AddNumberToObject(status, "total", st.total);
    cJSON_AddNumberToObject(status, "uploaded", st.uploaded);
    cJSON_AddNumberToObject(status, "failed", st.failed);
    cJSON_AddBoolToObject(status, "last_ok", st.last_ok);
    cJSON_AddStringToObject(status, "last_message", st.last_message);
    cJSON_AddBoolToObject(status, "last_test_ok", st.last_test_ok);
    cJSON_AddStringToObject(status, "last_test_message", st.last_test_message);
    cJSON_AddItemToObject(root, "status", status);

    cloud_sync_config_t cfg;
    if (cloud_sync_load(&cfg) == ESP_OK) {
        cJSON *health = cJSON_CreateObject();
        if (health) {
            cloud_sync_json_health(health, &cfg);
            cJSON_AddItemToObject(root, "health", health);
        }
    }
    return ESP_OK;
}
