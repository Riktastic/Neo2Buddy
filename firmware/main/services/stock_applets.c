/**
 * @file stock_applets.c
 * @brief Catalog of firmware-embedded stock SmartApplets + HTTP App Store API.
 */
#include "stock_applets.h"

#include "auth.h"
#include "neo_applet.h"
#include "usb_host_neo.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "stock_apps";

/*
 * Applet Store installs are paused until stock applets get more testing.
 * Binaries stay embedded; catalog/install stay dark. Flash Cards deck
 * push (stock_applets_flash_deck_write) is unaffected.
 */
#ifndef STOCK_STORE_INSTALLS_ENABLED
#define STOCK_STORE_INSTALLS_ENABLED 0
#endif

extern const uint8_t _binary_DiceTable_OS3KApp_start[];
extern const uint8_t _binary_DiceTable_OS3KApp_end[];
extern const uint8_t _binary_TaskPad_OS3KApp_start[];
extern const uint8_t _binary_TaskPad_OS3KApp_end[];
extern const uint8_t _binary_ScriptPad_OS3KApp_start[];
extern const uint8_t _binary_ScriptPad_OS3KApp_end[];
extern const uint8_t _binary_WordTree_OS3KApp_start[];
extern const uint8_t _binary_WordTree_OS3KApp_end[];
extern const uint8_t _binary_TypingDrill_OS3KApp_start[];
extern const uint8_t _binary_TypingDrill_OS3KApp_end[];
extern const uint8_t _binary_FlashCards_OS3KApp_start[];
extern const uint8_t _binary_FlashCards_OS3KApp_end[];
extern const uint8_t _binary_MathDrill_OS3KApp_start[];
extern const uint8_t _binary_MathDrill_OS3KApp_end[];
extern const uint8_t _binary_Snake_OS3KApp_start[];
extern const uint8_t _binary_Snake_OS3KApp_end[];
extern const uint8_t _binary_HangWord_OS3KApp_start[];
extern const uint8_t _binary_HangWord_OS3KApp_end[];
extern const uint8_t _binary_TicTacToe_OS3KApp_start[];
extern const uint8_t _binary_TicTacToe_OS3KApp_end[];
extern const uint8_t _binary_TouchType_OS3KApp_start[];
extern const uint8_t _binary_TouchType_OS3KApp_end[];

#define STOCK_FLASH_CARDS_ID 0xA1B6
#define STOCK_FLASH_DECK_MAX 8192

typedef struct {
    const char *slug;
    const char *name;
    const char *blurb;
    const char *summary;
    const char *how_to;
    const char *category;
    uint16_t applet_id;
    uint8_t version_major;
    uint8_t version_minor;
    char version_rev;
    const uint8_t *start;
    const uint8_t *end;
} stock_entry_t;

static const stock_entry_t s_catalog[] __attribute__((used)) = {
    { "dice-table", "Dice Table",
      "D&D dice + session notes. F1-F7 roll; Space=2d6; F8 undo. Auto-saves.",
      "Tabletop companion: roll dice on the right while keeping short session notes on the left.",
      "F1-F7 roll d4-d100. Space=2d6. F8 undo. Enter note. Find save. Clear File wipe.",
      "game", 0xA1B2, 1, 0, 'a',
      _binary_DiceTable_OS3KApp_start, _binary_DiceTable_OS3KApp_end },
    { "snake", "Snake",
      "Eat stars, grow faster, beat your best. Space pauses.",
      "Classic snake that speeds up as you score. Pause anytime; best score is saved.",
      "Enter start. Arrows steer (same again=nudge). Space pause. Clear File reset best.",
      "game", 0xA1B8, 1, 0, 'a',
      _binary_Snake_OS3KApp_start, _binary_Snake_OS3KApp_end },
    { "hang-word", "Hang Word",
      "Guess the secret word. Tab gives a hint (costs a life).",
      "Hangman with streak tracking, 40 words, and optional hints.",
      "Type letters. Tab=hint. Enter/Find=new word. Clear File reset score.",
      "game", 0xA1B9, 1, 0, 'a',
      _binary_HangWord_OS3KApp_start, _binary_HangWord_OS3KApp_end },
    { "tic-tac-toe", "Tic Tac Toe",
      "3x3 vs Neo. Tab toggles Easy/Hard. Keys 1-9 place instantly.",
      "Beat Neo on Easy or Hard. Use arrows+Enter or the number pad.",
      "Arrows+Enter or keys 1-9. Tab=AI level. Find=new. Clear File reset.",
      "game", 0xA1BA, 1, 0, 'a',
      _binary_TicTacToe_OS3KApp_start, _binary_TicTacToe_OS3KApp_end },
    { "task-pad", "Task Pad",
      "Checklist: Enter add, Space toggle, Delete remove. Find saves; Clear File wipes.",
      "A tiny checklist for errands, packing lists, or writing goals — saved on the Neo.",
      "Enter add. Space toggle done. Delete remove. Find save. Clear File wipe all.",
      "organize", 0xA1B1, 1, 0, 'a',
      _binary_TaskPad_OS3KApp_start, _binary_TaskPad_OS3KApp_end },
    { "script-pad", "Script Pad",
      "Screenplay lines. Tab inserts the next speaker cue; Enter adds a line. Auto-saves.",
      "Sketch dialogue and screenplay beats with rotating speaker cues.",
      "Enter new line. Tab inserts next speaker (ALICE:, BOB:…). Find save. Clear File reset.",
      "write", 0xA1B3, 1, 0, 'a',
      _binary_ScriptPad_OS3KApp_start, _binary_ScriptPad_OS3KApp_end },
    { "word-tree", "Word Tree",
      "Counts AlphaWord words on the Neo. Enter sets goal; Find refreshes the tree.",
      "Reads your AlphaWord files on-device and grows an ASCII tree toward a word goal.",
      "Enter set weekly goal. Find refresh counts from AlphaWord. Clear File reset goal.",
      "focus", 0xA1B4, 1, 0, 'a',
      _binary_WordTree_OS3KApp_start, _binary_WordTree_OS3KApp_end },
    { "type-drill", "Type Drill",
      "Timed prompt drill: WPM + accuracy. Find peeks at the next prompt.",
      "Short prompt races that measure typing speed and accuracy. Best WPM is saved.",
      "Enter start. Find next prompt. Type exactly. Esc cancel. Clear File reset best.",
      "focus", 0xA1B5, 1, 0, 'a',
      _binary_TypingDrill_OS3KApp_start, _binary_TypingDrill_OS3KApp_end },
    { "touch-type", "Touch Type",
      "Learn touch typing: 5 lessons, finger hints, live WPM — wrong keys don't advance.",
      "A coach for home-row habits. Progressive exercises with realtime feedback and finger cues.",
      "Tab pick lesson. Enter start. Type the line (misses stay put). Esc menu. Find skip.",
      "focus", 0xA1BB, 1, 0, 'a',
      _binary_TouchType_OS3KApp_start, _binary_TouchType_OS3KApp_end },
    { "flash-cards", "Flash Cards",
      "English→Dutch starter + multi-set editor in the portal. Reverse & type modes on Neo.",
      "Edit named decks in the portal, push one to the Neo. On-device: Show, Reverse, Type, Type↔ with hints.",
      "Tab cycle modes. Show/Reverse: Space+Y/N. Type: Enter check, Find=hint. Clear File=Dutch starter.",
      "learn", STOCK_FLASH_CARDS_ID, 1, 0, 'a',
      _binary_FlashCards_OS3KApp_start, _binary_FlashCards_OS3KApp_end },
    { "math-drill", "Math Drill",
      "Random arithmetic, algebra, and unit drills with streak celebrations.",
      "Text-only math and science practice with immediate feedback and best streak.",
      "Enter answer. Tab cycle Mix/Arith/Algebra/Units. Find skip. Clear File reset.",
      "learn", 0xA1B7, 1, 0, 'a',
      _binary_MathDrill_OS3KApp_start, _binary_MathDrill_OS3KApp_end },
};

#if STOCK_STORE_INSTALLS_ENABLED
static size_t entry_len(const stock_entry_t *e)
{
    if (!e || !e->start || !e->end || e->end <= e->start) {
        return 0;
    }
    return (size_t)(e->end - e->start);
}

static const stock_entry_t *find_entry(const char *slug)
{
    if (!slug) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_catalog) / sizeof(s_catalog[0]); i++) {
        if (strcmp(s_catalog[i].slug, slug) == 0) {
            return &s_catalog[i];
        }
    }
    return NULL;
}
#endif

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

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *code)
{
    char body[80];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

size_t stock_applets_count(void)
{
#if !STOCK_STORE_INSTALLS_ENABLED
    return 0;
#else
    size_t n = 0;
    for (size_t i = 0; i < sizeof(s_catalog) / sizeof(s_catalog[0]); i++) {
        if (entry_len(&s_catalog[i]) >= NEO_APPLET_HEADER_SIZE) {
            n++;
        }
    }
    return n;
#endif
}

esp_err_t stock_applets_install(const char *slug, bool replace)
{
#if !STOCK_STORE_INSTALLS_ENABLED
    (void)slug;
    (void)replace;
    ESP_LOGW(TAG, "Stock store installs disabled (testing hold)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const stock_entry_t *e = find_entry(slug);
    size_t len;
    if (!e) {
        return ESP_ERR_NOT_FOUND;
    }
    len = entry_len(e);
    if (len < NEO_APPLET_HEADER_SIZE) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!usb_host_neo_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Installing stock applet %s (%u bytes)", slug, (unsigned)len);
    return usb_host_neo_install_applet(e->start, len, replace);
#endif
}

static esp_err_t stock_list_handler(httpd_req_t *req)
{
    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *apps = cJSON_AddArrayToObject(root, "applets");
    size_t bundled = 0;
#if STOCK_STORE_INSTALLS_ENABLED
    for (size_t i = 0; i < sizeof(s_catalog) / sizeof(s_catalog[0]); i++) {
        size_t len = entry_len(&s_catalog[i]);
        cJSON *o = cJSON_CreateObject();
        char rev[2] = { s_catalog[i].version_rev, 0 };
        cJSON_AddStringToObject(o, "slug", s_catalog[i].slug);
        cJSON_AddStringToObject(o, "name", s_catalog[i].name);
        cJSON_AddStringToObject(o, "blurb", s_catalog[i].blurb);
        cJSON_AddStringToObject(o, "summary", s_catalog[i].summary);
        cJSON_AddStringToObject(o, "how_to", s_catalog[i].how_to);
        cJSON_AddStringToObject(o, "category", s_catalog[i].category);
        cJSON_AddNumberToObject(o, "applet_id", s_catalog[i].applet_id);
        cJSON_AddNumberToObject(o, "version_major", s_catalog[i].version_major);
        cJSON_AddNumberToObject(o, "version_minor", s_catalog[i].version_minor);
        cJSON_AddStringToObject(o, "version_rev", rev);
        cJSON_AddNumberToObject(o, "bytes", (double)len);
        cJSON_AddBoolToObject(o, "bundled", len >= NEO_APPLET_HEADER_SIZE);
        if (len >= NEO_APPLET_HEADER_SIZE) {
            bundled++;
        }
        cJSON_AddItemToArray(apps, o);
    }
#else
    (void)apps;
    cJSON_AddBoolToObject(root, "installs_enabled", false);
    cJSON_AddStringToObject(root, "note", "Applet Store installs paused for testing");
#endif
    cJSON_AddNumberToObject(root, "bundled_count", (double)bundled);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return send_json_error(req, "500 Internal Server Error", "json");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, strlen(printed));
    free(printed);
    return ESP_OK;
}

static esp_err_t stock_install_handler(httpd_req_t *req)
{
    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
#if !STOCK_STORE_INSTALLS_ENABLED
    return send_json_error(req, "503 Service Unavailable", "store_paused");
#else
    char slug[32];
    esp_err_t err;

    if (sscanf(req->uri, "/api/v1/neo/stock-applets/%31[^/]/install", slug) != 1) {
        return send_json_error(req, "400 Bad Request", "bad_slug");
    }
    err = stock_applets_install(slug, true);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "not_bundled");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "400 Bad Request", "neo_not_connected");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_json_error(req, "409 Conflict", "insufficient_space");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_CRC) {
        return send_json_error(req, "400 Bad Request", "invalid_package");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    (void)usb_host_neo_restart();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
#endif
}

esp_err_t stock_applets_flash_deck_write(const uint8_t *data, size_t length)
{
    if (!data || length == 0 || length > STOCK_FLASH_DECK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Writing Flash Cards deck (%u bytes)", (unsigned)length);
    /* Raw ASCII — not AlphaWord charmap. File space "1" = applet file 1. */
    return usb_host_neo_write_file_by_name(STOCK_FLASH_CARDS_ID, "1", "write", data, length);
}

static esp_err_t stock_flash_deck_handler(httpd_req_t *req)
{
    uint8_t *body = NULL;
    size_t body_len = 0;
    int total;
    int received = 0;
    esp_err_t err;

    if (!req_authenticated(req)) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    total = req->content_len;
    if (total <= 0 || total > STOCK_FLASH_DECK_MAX) {
        return send_json_error(req, "400 Bad Request", "deck_too_large");
    }
    body = malloc((size_t)total + 1);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "mem");
    }
    while (received < total) {
        int chunk = httpd_req_recv(req, (char *)body + received, (size_t)(total - received));
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (chunk <= 0) {
            free(body);
            return send_json_error(req, "400 Bad Request", "bad_body");
        }
        received += chunk;
    }
    body[total] = '\0';
    body_len = (size_t)total;

    /* Strip UTF-8 BOM if present. */
    if (body_len >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) {
        memmove(body, body + 3, body_len - 3);
        body_len -= 3;
        body[body_len] = '\0';
    }

    err = stock_applets_flash_deck_write(body, body_len);
    free(body);
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

esp_err_t stock_applets_web_register(httpd_handle_t server)
{
    httpd_uri_t list_uri = {
        .uri = "/api/v1/neo/stock-applets",
        .method = HTTP_GET,
        .handler = stock_list_handler,
        .user_ctx = NULL,
    };
    /* Register flash-cards deck route before the slug install wildcard. */
    httpd_uri_t deck_uri = {
        .uri = "/api/v1/neo/stock-applets/flash-cards/deck",
        .method = HTTP_POST,
        .handler = stock_flash_deck_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t install_uri = {
        .uri = "/api/v1/neo/stock-applets/*/install",
        .method = HTTP_POST,
        .handler = stock_install_handler,
        .user_ctx = NULL,
    };
    esp_err_t r = httpd_register_uri_handler(server, &list_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &deck_uri);
    if (r != ESP_OK) {
        return r;
    }
    return httpd_register_uri_handler(server, &install_uri);
}
