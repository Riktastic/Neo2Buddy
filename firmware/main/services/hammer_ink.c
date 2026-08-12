/**
 * @file hammer_ink.c
 * @brief Minimal Hammer sync protocol v3 client (hammer.ink official server).
 */

#include "hammer_ink.h"

#include "file_manager.h"
#include "log_buffer.h"
#include "neo_import.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "hammer_ink";

#define HAMMER_NVS_NS "hammer_ink"
#define HAMMER_HTTP_TIMEOUT_MS 90000
#define HAMMER_RESP_MAX (48 * 1024)
#define HAMMER_INSTALL_ID_MAX 40
#define HAMMER_TOKEN_MAX 96
#define HAMMER_PROJECT_ID_MAX 48
#define HAMMER_SYNC_ID_MAX 80
#define HAMMER_MAP_NAME "hammer_notes.map"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} hammer_buf_t;

typedef struct {
    long user_id;
    char auth[HAMMER_TOKEN_MAX];
    char refresh[HAMMER_TOKEN_MAX];
    char install_id[HAMMER_INSTALL_ID_MAX];
    char project_id[HAMMER_PROJECT_ID_MAX];
} hammer_session_t;

static void hammer_trim_slash(char *s)
{
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') {
        s[--n] = '\0';
    }
}

static esp_err_t hammer_url_encode(const char *in, char *out, size_t out_size)
{
    static const char *hex = "0123456789ABCDEF";
    if (!in || !out || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = (char)c;
        } else if (c == ' ') {
            if (o + 1 >= out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = '+';
        } else {
            if (o + 3 >= out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
    return ESP_OK;
}

static esp_err_t hammer_ensure_install_id(char *out, size_t out_size)
{
    if (!out || out_size < 33) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(HAMMER_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = out_size;
    err = nvs_get_str(h, "install_id", out, &len);
    if (err == ESP_OK && out[0] != '\0') {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, out_size - (size_t)(i * 2), "%02x", rnd[i]);
    }
    err = nvs_set_str(h, "install_id", out);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t hammer_load_project_id(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(HAMMER_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;
    }
    size_t len = out_size;
    nvs_get_str(h, "project_id", out, &len);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t hammer_save_project_id(const char *project_id)
{
    if (!project_id) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(HAMMER_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "project_id", project_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t hammer_map_path(char *out, size_t out_size)
{
    return file_manager_resolve_path(HAMMER_MAP_NAME, out, out_size);
}

static int hammer_map_lookup(const char *filename)
{
    char path[320];
    if (hammer_map_path(path, sizeof(path)) != ESP_OK) {
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    char line[128];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char name[FILE_MANAGER_NAME_MAX + 1];
        int id = 0;
        if (sscanf(line, "%48s %d", name, &id) == 2 && strcmp(name, filename) == 0 && id > 0) {
            found = id;
            break;
        }
    }
    fclose(f);
    return found;
}

static int hammer_map_max_id(void)
{
    char path[320];
    if (hammer_map_path(path, sizeof(path)) != ESP_OK) {
        return 0;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char line[128];
    int max_id = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[FILE_MANAGER_NAME_MAX + 1];
        int id = 0;
        if (sscanf(line, "%48s %d", name, &id) == 2 && id > max_id) {
            max_id = id;
        }
    }
    fclose(f);
    return max_id;
}

static esp_err_t hammer_map_set(const char *filename, int entity_id)
{
    if (!filename || entity_id <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (file_manager_ensure_dir() != ESP_OK) {
        return ESP_FAIL;
    }
    char path[320];
    if (hammer_map_path(path, sizeof(path)) != ESP_OK) {
        return ESP_FAIL;
    }

    char tmp[336];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in) {
            fclose(in);
        }
        return ESP_FAIL;
    }
    bool replaced = false;
    if (in) {
        char line[128];
        while (fgets(line, sizeof(line), in)) {
            char name[FILE_MANAGER_NAME_MAX + 1];
            int id = 0;
            if (sscanf(line, "%48s %d", name, &id) == 2) {
                if (strcmp(name, filename) == 0) {
                    fprintf(out, "%s %d\n", filename, entity_id);
                    replaced = true;
                } else {
                    fprintf(out, "%s %d\n", name, id);
                }
            }
        }
        fclose(in);
    }
    if (!replaced) {
        fprintf(out, "%s %d\n", filename, entity_id);
    }
    fclose(out);
    remove(path);
    if (rename(tmp, path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t hammer_buf_init(hammer_buf_t *b, size_t cap)
{
    memset(b, 0, sizeof(*b));
    b->data = calloc(1, cap);
    if (!b->data) {
        return ESP_ERR_NO_MEM;
    }
    b->cap = cap;
    return ESP_OK;
}

static void hammer_buf_free(hammer_buf_t *b)
{
    if (b && b->data) {
        free(b->data);
        b->data = NULL;
        b->len = 0;
        b->cap = 0;
    }
}

static esp_err_t hammer_buf_append(hammer_buf_t *b, const char *chunk, int len)
{
    if (!b || !chunk || len <= 0) {
        return ESP_OK;
    }
    if (b->len + (size_t)len + 1 > b->cap) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(b->data + b->len, chunk, (size_t)len);
    b->len += (size_t)len;
    b->data[b->len] = '\0';
    return ESP_OK;
}

typedef struct {
    const char *bearer;
    const char *sync_id;
    const char *entity_type;
    bool force;
} hammer_hdr_t;

static esp_err_t hammer_http(const char *method, const char *url, const char *content_type, const char *body,
                             size_t body_len, const hammer_hdr_t *hdr, hammer_buf_t *resp, int *out_status,
                             char *err, size_t err_size)
{
    if (!method || !url) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "https://", 8) != 0) {
        snprintf(err, err_size, "HTTPS required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HAMMER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    if (strcmp(method, "GET") == 0) {
        cfg.method = HTTP_METHOD_GET;
    } else {
        cfg.method = HTTP_METHOD_POST;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(err, err_size, "http init failed");
        return ESP_ERR_NO_MEM;
    }

    char proto[8];
    snprintf(proto, sizeof(proto), "%d", HAMMER_INK_PROTOCOL_VERSION);
    esp_http_client_set_header(client, "X-Hammer-Protocol-Version", proto);
    esp_http_client_set_header(client, "X-Client-Version", "neo2buddy/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    if (hdr) {
        if (hdr->bearer && hdr->bearer[0]) {
            char auth[HAMMER_TOKEN_MAX + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", hdr->bearer);
            esp_http_client_set_header(client, "Authorization", auth);
        }
        if (hdr->sync_id && hdr->sync_id[0]) {
            esp_http_client_set_header(client, "X-Sync-Id", hdr->sync_id);
        }
        if (hdr->entity_type && hdr->entity_type[0]) {
            esp_http_client_set_header(client, "X-Entity-Type", hdr->entity_type);
        }
    }
    if (content_type && content_type[0]) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }

    esp_err_t ret = esp_http_client_open(client, (int)body_len);
    if (ret != ESP_OK) {
        snprintf(err, err_size, "connection failed");
        esp_http_client_cleanup(client);
        return ret;
    }
    if (body_len > 0 && body) {
        int w = esp_http_client_write(client, body, (int)body_len);
        if (w < 0 || (size_t)w != body_len) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(err, err_size, "write failed");
            return ESP_FAIL;
        }
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status) {
        *out_status = status;
    }

    if (resp) {
        char chunk[512];
        while (1) {
            int r = esp_http_client_read(client, chunk, sizeof(chunk));
            if (r < 0) {
                break;
            }
            if (r == 0) {
                break;
            }
            if (hammer_buf_append(resp, chunk, r) != ESP_OK) {
                ESP_LOGW(TAG, "response truncated at %u bytes (cl=%d)", (unsigned)resp->len, content_length);
                break;
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        return ESP_OK;
    }
    if (status == 401) {
        snprintf(err, err_size, "Unauthorized — check hammer.ink email/password");
    } else if (status == 403) {
        snprintf(err, err_size, "Forbidden — account may need Patreon whitelist access");
    } else if (status == 409) {
        snprintf(err, err_size, "Conflict");
    } else if (status > 0) {
        snprintf(err, err_size, "HTTP %d", status);
    } else {
        snprintf(err, err_size, "No HTTP response");
    }
    return ESP_FAIL;
}

static esp_err_t hammer_login(const hammer_ink_config_t *cfg, hammer_session_t *sess, char *err, size_t err_size)
{
    memset(sess, 0, sizeof(*sess));
    if (hammer_ensure_install_id(sess->install_id, sizeof(sess->install_id)) != ESP_OK) {
        snprintf(err, err_size, "install id failed");
        return ESP_FAIL;
    }

    char email_enc[256];
    char pass_enc[384];
    char install_enc[96];
    if (hammer_url_encode(cfg->email, email_enc, sizeof(email_enc)) != ESP_OK ||
        hammer_url_encode(cfg->password, pass_enc, sizeof(pass_enc)) != ESP_OK ||
        hammer_url_encode(sess->install_id, install_enc, sizeof(install_enc)) != ESP_OK) {
        snprintf(err, err_size, "encode failed");
        return ESP_ERR_INVALID_SIZE;
    }

    char form[768];
    snprintf(form, sizeof(form), "email=%s&password=%s&installId=%s", email_enc, pass_enc, install_enc);

    char base[256];
    strlcpy(base, cfg->endpoint, sizeof(base));
    hammer_trim_slash(base);

    char url[320];
    snprintf(url, sizeof(url), "%s/api/account/login", base);

    hammer_buf_t resp;
    if (hammer_buf_init(&resp, 2048) != ESP_OK) {
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t http = hammer_http("POST", url, "application/x-www-form-urlencoded", form, strlen(form), NULL, &resp,
                                 &status, err, err_size);
    if (http != ESP_OK) {
        hammer_buf_free(&resp);
        return http;
    }

    cJSON *root = cJSON_Parse(resp.data);
    hammer_buf_free(&resp);
    if (!root) {
        snprintf(err, err_size, "bad login JSON");
        return ESP_FAIL;
    }
    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(root, "userId");
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "auth");
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(root, "refresh");
    if (!cJSON_IsNumber(uid) || !cJSON_IsString(auth) || !cJSON_IsString(refresh)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "login response incomplete");
        return ESP_FAIL;
    }
    sess->user_id = (long)uid->valuedouble;
    strlcpy(sess->auth, auth->valuestring, sizeof(sess->auth));
    strlcpy(sess->refresh, refresh->valuestring, sizeof(sess->refresh));
    cJSON_Delete(root);
    ESP_LOGI(TAG, "logged in userId=%ld", sess->user_id);
    return ESP_OK;
}

static esp_err_t hammer_account_begin(const char *endpoint, hammer_session_t *sess, char *sync_id, size_t sync_id_size,
                                      cJSON **projects_out, char *err, size_t err_size)
{
    char base[256];
    strlcpy(base, endpoint, sizeof(base));
    hammer_trim_slash(base);
    char url[320];
    snprintf(url, sizeof(url), "%s/api/projects/%ld/begin_sync", base, sess->user_id);

    hammer_buf_t resp;
    if (hammer_buf_init(&resp, HAMMER_RESP_MAX) != ESP_OK) {
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }
    hammer_hdr_t hdr = {.bearer = sess->auth};
    int status = 0;
    esp_err_t http = hammer_http("POST", url, NULL, NULL, 0, &hdr, &resp, &status, err, err_size);
    if (http != ESP_OK) {
        hammer_buf_free(&resp);
        return http;
    }

    cJSON *root = cJSON_Parse(resp.data);
    hammer_buf_free(&resp);
    if (!root) {
        snprintf(err, err_size, "bad begin_sync JSON");
        return ESP_FAIL;
    }
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "syncId");
    if (!cJSON_IsString(sid) || !sid->valuestring) {
        cJSON_Delete(root);
        snprintf(err, err_size, "missing syncId");
        return ESP_FAIL;
    }
    strlcpy(sync_id, sid->valuestring, sync_id_size);

    cJSON *projects = cJSON_DetachItemFromObjectCaseSensitive(root, "projects");
    cJSON_Delete(root);
    if (!projects) {
        projects = cJSON_CreateArray();
    }
    *projects_out = projects;
    return ESP_OK;
}

static esp_err_t hammer_account_end(const char *endpoint, hammer_session_t *sess, const char *sync_id, char *err,
                                    size_t err_size)
{
    char base[256];
    strlcpy(base, endpoint, sizeof(base));
    hammer_trim_slash(base);
    char url[320];
    snprintf(url, sizeof(url), "%s/api/projects/%ld/end_sync", base, sess->user_id);
    hammer_hdr_t hdr = {.bearer = sess->auth, .sync_id = sync_id};
    int status = 0;
    return hammer_http("POST", url, NULL, NULL, 0, &hdr, NULL, &status, err, err_size);
}

static bool hammer_find_project(cJSON *projects, const char *name, char *project_id, size_t project_id_size)
{
    if (!cJSON_IsArray(projects) && !cJSON_IsObject(projects)) {
        return false;
    }
    /* Set may serialize as array; tolerate either. */
    cJSON *it = NULL;
    if (cJSON_IsArray(projects)) {
        cJSON_ArrayForEach(it, projects)
        {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name");
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "uuid");
            if (cJSON_IsString(n) && cJSON_IsString(u) && strcmp(n->valuestring, name) == 0) {
                strlcpy(project_id, u->valuestring, project_id_size);
                return true;
            }
        }
    }
    return false;
}

static esp_err_t hammer_create_project(const char *endpoint, hammer_session_t *sess, const char *sync_id,
                                       const char *name, char *project_id, size_t project_id_size, char *err,
                                       size_t err_size)
{
    char name_enc[192];
    if (hammer_url_encode(name, name_enc, sizeof(name_enc)) != ESP_OK) {
        snprintf(err, err_size, "project name encode failed");
        return ESP_ERR_INVALID_SIZE;
    }
    char base[256];
    strlcpy(base, endpoint, sizeof(base));
    hammer_trim_slash(base);
    char url[520];
    snprintf(url, sizeof(url), "%s/api/projects/%ld/create?projectName=%s", base, sess->user_id, name_enc);

    hammer_buf_t resp;
    if (hammer_buf_init(&resp, 2048) != ESP_OK) {
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }
    hammer_hdr_t hdr = {.bearer = sess->auth, .sync_id = sync_id};
    int status = 0;
    esp_err_t http = hammer_http("POST", url, NULL, NULL, 0, &hdr, &resp, &status, err, err_size);
    if (http != ESP_OK) {
        hammer_buf_free(&resp);
        return http;
    }
    cJSON *root = cJSON_Parse(resp.data);
    hammer_buf_free(&resp);
    if (!root) {
        snprintf(err, err_size, "bad create project JSON");
        return ESP_FAIL;
    }
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(root, "projectId");
    if (!cJSON_IsString(pid) || !pid->valuestring) {
        cJSON_Delete(root);
        snprintf(err, err_size, "missing projectId");
        return ESP_FAIL;
    }
    strlcpy(project_id, pid->valuestring, project_id_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t hammer_ensure_project(const hammer_ink_config_t *cfg, hammer_session_t *sess, char *err,
                                       size_t err_size)
{
    const char *pname = (cfg->project_name && cfg->project_name[0]) ? cfg->project_name : HAMMER_INK_DEFAULT_PROJECT;

    hammer_load_project_id(sess->project_id, sizeof(sess->project_id));

    char sync_id[HAMMER_SYNC_ID_MAX];
    cJSON *projects = NULL;
    esp_err_t errc = hammer_account_begin(cfg->endpoint, sess, sync_id, sizeof(sync_id), &projects, err, err_size);
    if (errc != ESP_OK) {
        return errc;
    }

    bool found = false;
    if (sess->project_id[0]) {
        /* Prefer cached id if still listed. */
        cJSON *it = NULL;
        if (cJSON_IsArray(projects)) {
            cJSON_ArrayForEach(it, projects)
            {
                const cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "uuid");
                if (cJSON_IsString(u) && strcmp(u->valuestring, sess->project_id) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        found = hammer_find_project(projects, pname, sess->project_id, sizeof(sess->project_id));
    }
    if (!found) {
        errc = hammer_create_project(cfg->endpoint, sess, sync_id, pname, sess->project_id, sizeof(sess->project_id),
                                     err, err_size);
        if (errc != ESP_OK) {
            cJSON_Delete(projects);
            hammer_account_end(cfg->endpoint, sess, sync_id, err, err_size);
            return errc;
        }
        log_buffer_appendf("hammer: created project '%s'", pname);
    }
    cJSON_Delete(projects);
    hammer_save_project_id(sess->project_id);

    errc = hammer_account_end(cfg->endpoint, sess, sync_id, err, err_size);
    return errc;
}

static esp_err_t hammer_project_begin(const char *endpoint, hammer_session_t *sess, char *sync_id, size_t sync_id_size,
                                      int *last_id, char *err, size_t err_size)
{
    char base[256];
    strlcpy(base, endpoint, sizeof(base));
    hammer_trim_slash(base);
    char url[400];
    snprintf(url, sizeof(url), "%s/api/project/%ld/%s/begin_sync", base, sess->user_id, sess->project_id);

    hammer_buf_t resp;
    if (hammer_buf_init(&resp, HAMMER_RESP_MAX) != ESP_OK) {
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }
    hammer_hdr_t hdr = {.bearer = sess->auth};
    int status = 0;
    esp_err_t http = hammer_http("POST", url, "application/octet-stream", NULL, 0, &hdr, &resp, &status, err, err_size);
    if (http != ESP_OK) {
        hammer_buf_free(&resp);
        return http;
    }
    cJSON *root = cJSON_Parse(resp.data);
    hammer_buf_free(&resp);
    if (!root) {
        snprintf(err, err_size, "bad project begin_sync JSON");
        return ESP_FAIL;
    }
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "syncId");
    const cJSON *lid = cJSON_GetObjectItemCaseSensitive(root, "lastId");
    if (!cJSON_IsString(sid) || !sid->valuestring) {
        cJSON_Delete(root);
        snprintf(err, err_size, "missing project syncId");
        return ESP_FAIL;
    }
    strlcpy(sync_id, sid->valuestring, sync_id_size);
    *last_id = cJSON_IsNumber(lid) ? (int)lid->valuedouble : -1;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t hammer_project_end(const char *endpoint, hammer_session_t *sess, const char *sync_id, int last_id,
                                    char *err, size_t err_size)
{
    char base[256];
    strlcpy(base, endpoint, sizeof(base));
    hammer_trim_slash(base);
    char url[400];
    snprintf(url, sizeof(url), "%s/api/project/%ld/%s/end_sync", base, sess->user_id, sess->project_id);

    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char iso[40];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char form[128];
    snprintf(form, sizeof(form), "lastSync=%s&lastId=%d", iso, last_id);

    hammer_hdr_t hdr = {.bearer = sess->auth, .sync_id = sync_id};
    int status = 0;
    return hammer_http("POST", url, "application/x-www-form-urlencoded", form, strlen(form), &hdr, NULL, &status, err,
                       err_size);
}

static char *hammer_json_escape_alloc(const char *text)
{
    if (!text) {
        text = "";
    }
    size_t need = 2;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\' || *p < 0x20) {
            need += 6;
        } else {
            need += 1;
        }
    }
    char *out = malloc(need + 8);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (*p == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (*p == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (*p < 0x20) {
            o += (size_t)snprintf(out + o, need + 8 - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
    return out;
}

static esp_err_t hammer_upload_note(const char *endpoint, hammer_session_t *sess, const char *sync_id, int entity_id,
                                    const char *filename, const char *content, bool force, char *err, size_t err_size)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char iso[40];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char *content_json = hammer_json_escape_alloc(content);
    if (!content_json) {
        snprintf(err, err_size, "no memory for note");
        return ESP_ERR_NO_MEM;
    }

    /* Prefix note content with filename so notes are identifiable in Hammer. */
    char header[96];
    snprintf(header, sizeof(header), "[Neo2 Buddy] %s\n\n", filename);
    char *prefixed = NULL;
    size_t plen = strlen(header) + strlen(content);
    prefixed = malloc(plen + 1);
    if (!prefixed) {
        free(content_json);
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }
    snprintf(prefixed, plen + 1, "%s%s", header, content);
    free(content_json);
    content_json = hammer_json_escape_alloc(prefixed);
    free(prefixed);
    if (!content_json) {
        snprintf(err, err_size, "no memory for note");
        return ESP_ERR_NO_MEM;
    }

    char *tag_json = hammer_json_escape_alloc(filename);
    if (!tag_json) {
        free(content_json);
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }

    size_t body_cap = strlen(content_json) + strlen(tag_json) + 256;
    char *body = malloc(body_cap);
    if (!body) {
        free(content_json);
        free(tag_json);
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, body_cap,
             "{\"type\":\"NOTE\",\"id\":%d,\"content\":%s,\"created\":\"%s\",\"tags\":[\"neo2buddy\",%s]}", entity_id,
             content_json, iso, tag_json);
    free(content_json);
    free(tag_json);

    char base[256];
    strlcpy(base, endpoint, sizeof(base));
    hammer_trim_slash(base);
    char url[420];
    if (force) {
        snprintf(url, sizeof(url), "%s/api/project/%ld/%s/upload_entity/%d?force=true", base, sess->user_id,
                 sess->project_id, entity_id);
    } else {
        snprintf(url, sizeof(url), "%s/api/project/%ld/%s/upload_entity/%d", base, sess->user_id, sess->project_id,
                 entity_id);
    }

    hammer_hdr_t hdr = {.bearer = sess->auth, .sync_id = sync_id, .entity_type = "note", .force = force};
    int status = 0;
    esp_err_t http = hammer_http("POST", url, "application/json", body, strlen(body), &hdr, NULL, &status, err, err_size);
    free(body);

    if (http != ESP_OK && status == 409 && !force) {
        return hammer_upload_note(endpoint, sess, sync_id, entity_id, filename, content, true, err, err_size);
    }
    return http;
}

static esp_err_t hammer_read_file(const char *filename, char **out_text, size_t *out_len, char *err, size_t err_size)
{
    char path[320];
    if (file_manager_resolve_path(filename, path, sizeof(path)) != ESP_OK) {
        snprintf(err, err_size, "bad name");
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "open failed");
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
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        snprintf(err, err_size, "no memory");
        return ESP_ERR_NO_MEM;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        snprintf(err, err_size, "read failed");
        return ESP_FAIL;
    }
    fclose(f);
    buf[sz] = '\0';
    *out_text = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

esp_err_t hammer_ink_test(const hammer_ink_config_t *cfg, char *message, size_t message_size)
{
    if (!cfg || !cfg->endpoint || !cfg->email || !cfg->password || !message || message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    hammer_session_t sess;
    char err[128];
    esp_err_t rc = hammer_login(cfg, &sess, err, sizeof(err));
    if (rc != ESP_OK) {
        snprintf(message, message_size, "%s", err);
        return rc;
    }
    rc = hammer_ensure_project(cfg, &sess, err, sizeof(err));
    if (rc != ESP_OK) {
        snprintf(message, message_size, "%s", err);
        return rc;
    }
    snprintf(message, message_size, "Hammer login OK (project ready)");
    return ESP_OK;
}

esp_err_t hammer_ink_upload_backups(const hammer_ink_config_t *cfg, uint32_t *uploaded, uint32_t *failed,
                                    hammer_ink_progress_cb_t progress, void *progress_ctx, char *err, size_t err_size)
{
    if (!cfg || !cfg->endpoint || !cfg->email || !cfg->password) {
        snprintf(err, err_size, "invalid config");
        return ESP_ERR_INVALID_ARG;
    }
    if (uploaded) {
        *uploaded = 0;
    }
    if (failed) {
        *failed = 0;
    }

    hammer_session_t sess;
    esp_err_t rc = hammer_login(cfg, &sess, err, err_size);
    if (rc != ESP_OK) {
        return rc;
    }
    rc = hammer_ensure_project(cfg, &sess, err, err_size);
    if (rc != ESP_OK) {
        return rc;
    }

    char sync_id[HAMMER_SYNC_ID_MAX];
    int last_id = -1;
    rc = hammer_project_begin(cfg->endpoint, &sess, sync_id, sizeof(sync_id), &last_id, err, err_size);
    if (rc != ESP_OK) {
        return rc;
    }

    int map_max = hammer_map_max_id();
    if (map_max > last_id) {
        last_id = map_max;
    }

    if (file_manager_ensure_dir() != ESP_OK) {
        snprintf(err, err_size, "backup folder missing");
        hammer_project_end(cfg->endpoint, &sess, sync_id, last_id, err, err_size);
        return ESP_FAIL;
    }

    const char *base_path = file_manager_base_path();
    DIR *dir = opendir(base_path);
    if (!dir) {
        snprintf(err, err_size, "Cannot open backup folder");
        hammer_project_end(cfg->endpoint, &sess, sync_id, last_id, err, err_size);
        return ESP_FAIL;
    }

    /* Count first for progress. */
    uint8_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen > 4 && strcasecmp(ent->d_name + nlen - 4, ".txt") == 0 &&
            file_manager_validate_name(ent->d_name) == ESP_OK) {
            total++;
        }
    }
    rewinddir(dir);

    uint8_t current = 0;
    uint32_t up = 0;
    uint32_t fail = 0;
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
        current++;
        if (progress) {
            progress(current, total, ent->d_name, progress_ctx);
        }

        int eid = hammer_map_lookup(ent->d_name);
        bool is_new = false;
        if (eid <= 0) {
            last_id++;
            eid = last_id;
            is_new = true;
        }

        char *text = NULL;
        size_t tlen = 0;
        char file_err[96];
        if (hammer_read_file(ent->d_name, &text, &tlen, file_err, sizeof(file_err)) != ESP_OK) {
            fail++;
            snprintf(err, err_size, "%s: %s", ent->d_name, file_err);
            continue;
        }
        /* Skip empty / whitespace-only backups (pad-only AlphaWord slots). */
        if (tlen == 0 || neo_import_text_is_blank(text, tlen)) {
            free(text);
            continue;
        }

        /* New notes: no force. Existing: force overwrite to avoid needing Murmur hash. */
        esp_err_t up_err = hammer_upload_note(cfg->endpoint, &sess, sync_id, eid, ent->d_name, text, !is_new, file_err,
                                              sizeof(file_err));
        free(text);
        if (up_err == ESP_OK) {
            hammer_map_set(ent->d_name, eid);
            up++;
        } else {
            fail++;
            snprintf(err, err_size, "%s: %s", ent->d_name, file_err);
            ESP_LOGW(TAG, "upload %s failed: %s", ent->d_name, file_err);
        }
    }
    closedir(dir);

    char end_err[96];
    esp_err_t end_rc = hammer_project_end(cfg->endpoint, &sess, sync_id, last_id, end_err, sizeof(end_err));
    if (end_rc != ESP_OK) {
        ESP_LOGW(TAG, "project end_sync failed: %s", end_err);
        if (fail == 0) {
            snprintf(err, err_size, "uploads OK but end_sync failed: %s", end_err);
        }
    }

    if (uploaded) {
        *uploaded = up;
    }
    if (failed) {
        *failed = fail;
    }
    if (fail == 0 && end_rc == ESP_OK) {
        snprintf(err, err_size, "Uploaded %u note(s) to Hammer", (unsigned)up);
        return ESP_OK;
    }
    if (fail == 0) {
        return end_rc;
    }
    return ESP_FAIL;
}
