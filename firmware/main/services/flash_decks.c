/**
 * @file flash_decks.c
 * @brief Multi-deck flashcard library on buddy storage + REST API.
 */
#include "flash_decks.h"

#include "auth.h"
#include "file_manager.h"
#include "stock_applets.h"
#include "usb_host_neo.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "flash_decks";

#define FC_MAX_CARDS 16
#define FC_SIDE_LEN 24
#define FC_ID_LEN 24
#define FC_NAME_LEN 40
#define FC_FILE_MAX 4096
#define FC_PREFIX "fc_"
#define FC_SUFFIX ".json"

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *code)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", code ? code : "error");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}

static bool req_authenticated(httpd_req_t *req)
{
    char auth[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth, "Bearer ", 7) != 0) {
        return false;
    }
    return auth_check_token(auth + 7);
}

static void sanitize_id(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (!in || !out || out_sz < 2) {
        if (out && out_sz) {
            out[0] = '\0';
        }
        return;
    }
    for (; *in && o + 1 < out_sz && o < FC_ID_LEN - 1; in++) {
        char c = *in;
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            out[o++] = (char)tolower((unsigned char)c);
        } else if (c == ' ' && o > 0 && out[o - 1] != '-') {
            out[o++] = '-';
        }
    }
    out[o] = '\0';
    if (o == 0) {
        strncpy(out, "deck", out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

static void deck_path(const char *id, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "/spiflash/%s%s%s", FC_PREFIX, id, FC_SUFFIX);
}

static void clip_side(char *s)
{
    size_t n;
    if (!s) {
        return;
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    if (n >= FC_SIDE_LEN) {
        s[FC_SIDE_LEN - 1] = '\0';
    }
}

static cJSON *default_en_nl_deck(void)
{
    static const char *fronts[] = {
        "hello", "goodbye", "please", "thank you", "yes", "no", "good morning", "how are you?",
        "I", "you", "water", "bread", "milk", "house", "friend", "today"
    };
    static const char *backs[] = {
        "hallo", "tot ziens", "alsjeblieft", "dank je", "ja", "nee", "goedemorgen", "hoe gaat het?",
        "ik", "jij", "water", "brood", "melk", "huis", "vriend", "vandaag"
    };
    cJSON *root = cJSON_CreateObject();
    cJSON *cards = cJSON_AddArrayToObject(root, "cards");
    size_t i;
    cJSON_AddStringToObject(root, "id", "en-nl-basic");
    cJSON_AddStringToObject(root, "name", "English to Dutch");
    for (i = 0; i < sizeof(fronts) / sizeof(fronts[0]); i++) {
        cJSON *card = cJSON_CreateObject();
        cJSON_AddStringToObject(card, "front", fronts[i]);
        cJSON_AddStringToObject(card, "back", backs[i]);
        cJSON_AddItemToArray(cards, card);
    }
    return root;
}

static esp_err_t write_deck_file(const char *id, cJSON *root)
{
    char path[80];
    char *printed;
    FILE *f;
    size_t n;

    if (!id || !id[0] || !root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!file_manager_flash_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON_DeleteItemFromObject(root, "id");
    cJSON_AddStringToObject(root, "id", id);
    printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    n = strlen(printed);
    if (n > FC_FILE_MAX) {
        free(printed);
        return ESP_ERR_INVALID_SIZE;
    }
    deck_path(id, path, sizeof(path));
    f = fopen(path, "w");
    if (!f) {
        free(printed);
        return ESP_FAIL;
    }
    if (fwrite(printed, 1, n, f) != n) {
        fclose(f);
        free(printed);
        return ESP_FAIL;
    }
    fclose(f);
    free(printed);
    ESP_LOGI(TAG, "Saved deck %s (%u bytes)", id, (unsigned)n);
    return ESP_OK;
}

static cJSON *read_deck_file(const char *id)
{
    char path[80];
    char *buf;
    FILE *f;
    long sz;
    cJSON *root;

    deck_path(id, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz <= 0 || sz > FC_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static void ensure_seed_deck(void)
{
    char path[80];
    struct stat st;
    deck_path("en-nl-basic", path, sizeof(path));
    if (stat(path, &st) == 0) {
        return;
    }
    cJSON *deck = default_en_nl_deck();
    if (deck) {
        (void)write_deck_file("en-nl-basic", deck);
        cJSON_Delete(deck);
    }
}

static int count_cards(cJSON *root)
{
    cJSON *cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    if (!cJSON_IsArray(cards)) {
        return 0;
    }
    return cJSON_GetArraySize(cards);
}

static esp_err_t normalize_deck_inplace(cJSON *root)
{
    cJSON *cards;
    cJSON *name;
    int n;
    int i;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0]) {
        cJSON_DeleteItemFromObject(root, "name");
        cJSON_AddStringToObject(root, "name", "Untitled deck");
    }
    cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    if (!cJSON_IsArray(cards)) {
        return ESP_ERR_INVALID_ARG;
    }
    n = cJSON_GetArraySize(cards);
    if (n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n > FC_MAX_CARDS) {
        /* Trim extras. */
        while (cJSON_GetArraySize(cards) > FC_MAX_CARDS) {
            cJSON_DeleteItemFromArray(cards, cJSON_GetArraySize(cards) - 1);
        }
        n = FC_MAX_CARDS;
    }
    for (i = 0; i < n; i++) {
        cJSON *card = cJSON_GetArrayItem(cards, i);
        cJSON *front;
        cJSON *back;
        char fbuf[FC_SIDE_LEN];
        char bbuf[FC_SIDE_LEN];
        if (!cJSON_IsObject(card)) {
            return ESP_ERR_INVALID_ARG;
        }
        front = cJSON_GetObjectItemCaseSensitive(card, "front");
        back = cJSON_GetObjectItemCaseSensitive(card, "back");
        if (!cJSON_IsString(front) || !cJSON_IsString(back)) {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(fbuf, front->valuestring, sizeof(fbuf) - 1);
        fbuf[sizeof(fbuf) - 1] = '\0';
        strncpy(bbuf, back->valuestring, sizeof(bbuf) - 1);
        bbuf[sizeof(bbuf) - 1] = '\0';
        clip_side(fbuf);
        clip_side(bbuf);
        if (!fbuf[0] || !bbuf[0]) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_ReplaceItemInObject(card, "front", cJSON_CreateString(fbuf));
        cJSON_ReplaceItemInObject(card, "back", cJSON_CreateString(bbuf));
    }
    return ESP_OK;
}

static esp_err_t deck_to_neo_text(cJSON *root, char *out, size_t out_sz, size_t *out_len)
{
    cJSON *cards;
    int n;
    int i;
    size_t off = 0;

    cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    if (!cJSON_IsArray(cards)) {
        return ESP_ERR_INVALID_ARG;
    }
    n = cJSON_GetArraySize(cards);
    if (n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n > FC_MAX_CARDS) {
        n = FC_MAX_CARDS;
    }
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        cJSON *card = cJSON_GetArrayItem(cards, i);
        cJSON *front = cJSON_GetObjectItemCaseSensitive(card, "front");
        cJSON *back = cJSON_GetObjectItemCaseSensitive(card, "back");
        int need;
        if (!cJSON_IsString(front) || !cJSON_IsString(back)) {
            continue;
        }
        need = (int)strlen(front->valuestring) + (int)strlen(back->valuestring) + 2;
        if (off + (size_t)need + 1 >= out_sz) {
            break;
        }
        off += (size_t)snprintf(out + off, out_sz - off, "%s|%s\n", front->valuestring, back->valuestring);
    }
    if (out_len) {
        *out_len = off;
    }
    return off > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t read_body(httpd_req_t *req, char **out, size_t *out_len)
{
    int total = req->content_len;
    int received = 0;
    char *body;
    if (total <= 0 || total > FC_FILE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    body = malloc((size_t)total + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    while (received < total) {
        int chunk = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (chunk <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += chunk;
    }
    body[total] = '\0';
    *out = body;
    *out_len = (size_t)total;
    return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    cJSON *root;
    cJSON *arr;
    DIR *dir;
    struct dirent *ent;
    char *printed;

    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    ensure_seed_deck();
    root = cJSON_CreateObject();
    arr = cJSON_AddArrayToObject(root, "decks");
    dir = opendir("/spiflash");
    if (dir) {
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            char id[FC_ID_LEN];
            cJSON *deck;
            cJSON *o;
            cJSON *name;
            if (len < sizeof(FC_PREFIX) + sizeof(FC_SUFFIX) - 1) {
                continue;
            }
            if (strncmp(ent->d_name, FC_PREFIX, sizeof(FC_PREFIX) - 1) != 0) {
                continue;
            }
            if (strcmp(ent->d_name + len - (sizeof(FC_SUFFIX) - 1), FC_SUFFIX) != 0) {
                continue;
            }
            {
                size_t id_len = len - (sizeof(FC_PREFIX) - 1) - (sizeof(FC_SUFFIX) - 1);
                if (id_len >= sizeof(id)) {
                    id_len = sizeof(id) - 1;
                }
                memcpy(id, ent->d_name + sizeof(FC_PREFIX) - 1, id_len);
                id[id_len] = '\0';
            }
            deck = read_deck_file(id);
            if (!deck) {
                continue;
            }
            o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", id);
            name = cJSON_GetObjectItemCaseSensitive(deck, "name");
            cJSON_AddStringToObject(o, "name",
                                    cJSON_IsString(name) && name->valuestring ? name->valuestring : id);
            cJSON_AddNumberToObject(o, "cards", count_cards(deck));
            cJSON_AddItemToArray(arr, o);
            cJSON_Delete(deck);
        }
        closedir(dir);
    }
    printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return send_json_error(req, "500 Internal Server Error", "json");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, strlen(printed));
    free(printed);
    return ESP_OK;
}

static bool parse_id_from_uri(const char *uri, const char *suffix, char *id, size_t id_sz)
{
    const char *prefix = "/api/v1/flashdecks/";
    const char *p;
    const char *end;
    size_t n;
    if (!uri || strncmp(uri, prefix, strlen(prefix)) != 0) {
        return false;
    }
    p = uri + strlen(prefix);
    if (suffix && suffix[0]) {
        end = strstr(p, suffix);
        if (!end || end == p) {
            return false;
        }
    } else {
        end = p;
        while (*end && *end != '/' && *end != '?') {
            end++;
        }
        if (end == p) {
            return false;
        }
        /* Reject extra path segments for GET/PUT/DELETE of the deck itself. */
        if (*end == '/') {
            return false;
        }
    }
    n = (size_t)(end - p);
    if (n >= id_sz) {
        n = id_sz - 1;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    return id[0] != '\0';
}

static esp_err_t get_handler(httpd_req_t *req)
{
    char id[FC_ID_LEN];
    cJSON *deck;
    char *printed;

    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    if (!parse_id_from_uri(req->uri, NULL, id, sizeof(id))) {
        return send_json_error(req, "400 Bad Request", "bad_id");
    }
    deck = read_deck_file(id);
    if (!deck) {
        return send_json_error(req, "404 Not Found", "not_found");
    }
    printed = cJSON_PrintUnformatted(deck);
    cJSON_Delete(deck);
    if (!printed) {
        return send_json_error(req, "500 Internal Server Error", "json");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, strlen(printed));
    free(printed);
    return ESP_OK;
}

static esp_err_t put_handler(httpd_req_t *req)
{
    char id[FC_ID_LEN];
    char id_in[FC_ID_LEN];
    char *body = NULL;
    size_t body_len = 0;
    cJSON *root;
    esp_err_t err;

    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    if (!parse_id_from_uri(req->uri, NULL, id_in, sizeof(id_in))) {
        return send_json_error(req, "400 Bad Request", "bad_id");
    }
    sanitize_id(id_in, id, sizeof(id));
    err = read_body(req, &body, &body_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_json_error(req, "400 Bad Request", "too_large");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "bad_body");
    }
    root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_json_error(req, "400 Bad Request", "bad_json");
    }
    err = normalize_deck_inplace(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "invalid_deck");
    }
    err = write_deck_file(id, root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "save_failed");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char id[FC_ID_LEN];
    char path[80];

    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    if (!parse_id_from_uri(req->uri, NULL, id, sizeof(id))) {
        return send_json_error(req, "400 Bad Request", "bad_id");
    }
    if (strcmp(id, "en-nl-basic") == 0) {
        return send_json_error(req, "400 Bad Request", "protected");
    }
    deck_path(id, path, sizeof(path));
    if (unlink(path) != 0) {
        return send_json_error(req, "404 Not Found", "not_found");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t push_handler(httpd_req_t *req)
{
    char id[FC_ID_LEN];
    cJSON *deck;
    char text[FC_MAX_CARDS * (FC_SIDE_LEN * 2 + 2) + 8];
    size_t text_len = 0;
    esp_err_t err;

    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    if (!parse_id_from_uri(req->uri, "/push", id, sizeof(id))) {
        return send_json_error(req, "400 Bad Request", "bad_id");
    }
    deck = read_deck_file(id);
    if (!deck) {
        return send_json_error(req, "404 Not Found", "not_found");
    }
    err = deck_to_neo_text(deck, text, sizeof(text), &text_len);
    cJSON_Delete(deck);
    if (err != ESP_OK || text_len == 0) {
        return send_json_error(req, "400 Bad Request", "empty_deck");
    }
    err = stock_applets_flash_deck_write((const uint8_t *)text, text_len);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "400 Bad Request", "neo_not_connected");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    (void)usb_host_neo_restart();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t flash_decks_web_register(httpd_handle_t server)
{
    httpd_uri_t list_uri = {
        .uri = "/api/v1/flashdecks",
        .method = HTTP_GET,
        .handler = list_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t get_uri = {
        .uri = "/api/v1/flashdecks/*",
        .method = HTTP_GET,
        .handler = get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t put_uri = {
        .uri = "/api/v1/flashdecks/*",
        .method = HTTP_PUT,
        .handler = put_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t del_uri = {
        .uri = "/api/v1/flashdecks/*",
        .method = HTTP_DELETE,
        .handler = delete_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t push_uri = {
        .uri = "/api/v1/flashdecks/*/push",
        .method = HTTP_POST,
        .handler = push_handler,
        .user_ctx = NULL,
    };
    esp_err_t r = httpd_register_uri_handler(server, &list_uri);
    if (r != ESP_OK) return r;
    r = httpd_register_uri_handler(server, &push_uri);
    if (r != ESP_OK) return r;
    r = httpd_register_uri_handler(server, &get_uri);
    if (r != ESP_OK) return r;
    r = httpd_register_uri_handler(server, &put_uri);
    if (r != ESP_OK) return r;
    return httpd_register_uri_handler(server, &del_uri);
}
