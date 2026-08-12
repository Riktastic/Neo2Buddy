/**
 * @file neo_link_llm.c
 * @brief OpenAI-compatible /v1/chat/completions client for Neo Link.
 */

#include "neo_link_llm.h"

#include "auth.h"
#include "log_buffer.h"
#include "neo_link_mailbox.h"
#include "neo_link_protocol.h"
#include "neo_link_text.h"
#include "neo_link_transport.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "neo_link_llm";

#define NEO_LINK_LLM_NVS "neo_link"
#define NEO_LINK_LLM_HTTP_TIMEOUT_MS 60000
#define NEO_LINK_LLM_RESP_MAX 8192
#define NEO_LINK_LLM_TASK_STACK 12288
#define NEO_LINK_LLM_ERR_MAX 160
#define NEO_LINK_CTX_MSG_MAX 400
#define NEO_LINK_CTX_TURNS_MAX 2
#define NEO_LINK_RPM_SLOTS 32

static SemaphoreHandle_t s_busy;
static char s_last_error[NEO_LINK_LLM_ERR_MAX];

/** Prior turns kept only in RAM (Neo Link applet — never AlphaWord). */
static char s_ctx_user[NEO_LINK_CTX_TURNS_MAX][NEO_LINK_CTX_MSG_MAX];
static char s_ctx_asst[NEO_LINK_CTX_TURNS_MAX][NEO_LINK_CTX_MSG_MAX];
static uint8_t s_ctx_count;

static int64_t s_rpm_ms[NEO_LINK_RPM_SLOTS];
static uint8_t s_rpm_head;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} neo_link_llm_buf_t;

static void neo_link_llm_set_last_error(const char *msg)
{
    if (!msg || !msg[0]) {
        s_last_error[0] = '\0';
        return;
    }
    strlcpy(s_last_error, msg, sizeof(s_last_error));
}

/** Human-readable HTTPS failure for the portal (not just "connect failed"). */
static void neo_link_llm_format_connect_error(esp_err_t ret, char *err, size_t err_size)
{
    const unsigned free_h = (unsigned)esp_get_free_heap_size();
    const unsigned largest =
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    const unsigned spiram = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char *code = esp_err_to_name(ret);

    /* mbedtls_ssl_setup -0x7F00 = alloc failed; often surfaces as ESP_FAIL / HTTP_CONNECT. */
    if (ret == ESP_ERR_NO_MEM || largest < 24576) {
        snprintf(err, err_size,
                 "HTTPS/TLS out of memory (%s). DRAM largest=%u free=%u PSRAM=%u. "
                 "Need firmware with PSRAM + mbedtls external alloc.",
                 code, largest, free_h, spiram);
        return;
    }
    if (ret == ESP_ERR_HTTP_CONNECT || ret == ESP_FAIL) {
        snprintf(err, err_size,
                 "HTTPS connect failed (%s). Check base URL, DNS, and TLS memory "
                 "(DRAM largest=%u, PSRAM=%u).",
                 code, largest, spiram);
        return;
    }
    if (ret == ESP_ERR_HTTP_CONNECTION_CLOSED) {
        snprintf(err, err_size, "HTTPS closed by server early (%s)", code);
        return;
    }
    snprintf(err, err_size,
             "HTTPS connect failed: %s (DRAM largest=%u free=%u PSRAM=%u)",
             code, largest, free_h, spiram);
}

static uint16_t neo_link_llm_clamp_tokens(int v)
{
    if (v < 50) {
        return 50;
    }
    if (v > 800) {
        return 800;
    }
    return (uint16_t)v;
}

static uint8_t neo_link_llm_clamp_rpm(int v)
{
    if (v < 1) {
        return 1;
    }
    if (v > 30) {
        return 30;
    }
    return (uint8_t)v;
}

static uint8_t neo_link_llm_clamp_turns(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > NEO_LINK_CTX_TURNS_MAX) {
        return NEO_LINK_CTX_TURNS_MAX;
    }
    return (uint8_t)v;
}

void neo_link_llm_defaults(neo_link_llm_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    cfg->max_tokens = 450;
    cfg->max_rpm = 6;
    cfg->context_turns = 2;
    strlcpy(cfg->model, "gpt-5-nano", sizeof(cfg->model));
    strlcpy(cfg->system,
            "You are a concise assistant for an AlphaSmart Neo2 LCD (40 columns), "
            "shown only inside the Neo Link Chat applet (not AlphaWord). "
            "Reply in plain ASCII under 200 characters. Use short paragraphs and line breaks. "
            "No markdown, no bullet symbols, no emoji.",
            sizeof(cfg->system));
}

void neo_link_llm_clear_context(void)
{
    memset(s_ctx_user, 0, sizeof(s_ctx_user));
    memset(s_ctx_asst, 0, sizeof(s_ctx_asst));
    s_ctx_count = 0;
}

static void neo_link_llm_remember_turn(const char *prompt, const char *reply)
{
    if (!prompt || !reply) {
        return;
    }
    if (s_ctx_count >= NEO_LINK_CTX_TURNS_MAX) {
        memmove(s_ctx_user[0], s_ctx_user[1], sizeof(s_ctx_user[0]));
        memmove(s_ctx_asst[0], s_ctx_asst[1], sizeof(s_ctx_asst[0]));
        s_ctx_count = NEO_LINK_CTX_TURNS_MAX - 1;
    }
    strlcpy(s_ctx_user[s_ctx_count], prompt, sizeof(s_ctx_user[0]));
    strlcpy(s_ctx_asst[s_ctx_count], reply, sizeof(s_ctx_asst[0]));
    s_ctx_count++;
}

/** Check local RPM budget without consuming a slot (failed TLS must not burn quota). */
static bool neo_link_llm_rate_check(uint8_t max_rpm, char *err, size_t err_size)
{
    int64_t now = esp_timer_get_time() / 1000; /* ms */
    int64_t window = now - 60000;
    int count = 0;
    int64_t oldest = 0;
    for (uint8_t i = 0; i < NEO_LINK_RPM_SLOTS; i++) {
        if (s_rpm_ms[i] > window) {
            count++;
            if (oldest == 0 || s_rpm_ms[i] < oldest) {
                oldest = s_rpm_ms[i];
            }
        }
    }
    if (count >= (int)max_rpm) {
        int wait_s = 60;
        if (oldest > 0) {
            wait_s = (int)((oldest + 60000 - now + 999) / 1000);
            if (wait_s < 1) {
                wait_s = 1;
            }
            if (wait_s > 60) {
                wait_s = 60;
            }
        }
        snprintf(err, err_size,
                 "Buddy local limit: %u req/min used. Wait ~%ds or raise Max req/min (not OpenAI).",
                 (unsigned)max_rpm, wait_s);
        return false;
    }
    return true;
}

static void neo_link_llm_rate_commit(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    s_rpm_ms[s_rpm_head % NEO_LINK_RPM_SLOTS] = now;
    s_rpm_head++;
}

esp_err_t neo_link_llm_load(neo_link_llm_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_link_llm_defaults(cfg);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NEO_LINK_LLM_NVS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t enabled = 0;
    nvs_get_u8(h, "enabled", &enabled);
    cfg->enabled = enabled != 0;
    size_t len = sizeof(cfg->base_url);
    nvs_get_str(h, "base_url", cfg->base_url, &len);
    len = sizeof(cfg->api_key);
    nvs_get_str(h, "api_key", cfg->api_key, &len);
    len = sizeof(cfg->model);
    nvs_get_str(h, "model", cfg->model, &len);
    len = sizeof(cfg->system);
    nvs_get_str(h, "system", cfg->system, &len);
    uint16_t tokens = cfg->max_tokens;
    uint8_t rpm = cfg->max_rpm;
    uint8_t turns = cfg->context_turns;
    nvs_get_u16(h, "max_tokens", &tokens);
    nvs_get_u8(h, "max_rpm", &rpm);
    nvs_get_u8(h, "ctx_turns", &turns);
    cfg->max_tokens = neo_link_llm_clamp_tokens(tokens);
    cfg->max_rpm = neo_link_llm_clamp_rpm(rpm);
    cfg->context_turns = neo_link_llm_clamp_turns(turns);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t neo_link_llm_save(const neo_link_llm_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NEO_LINK_LLM_NVS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "base_url", cfg->base_url);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "api_key", cfg->api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "model", cfg->model);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "system", cfg->system);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "max_tokens", neo_link_llm_clamp_tokens(cfg->max_tokens));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "max_rpm", neo_link_llm_clamp_rpm(cfg->max_rpm));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "ctx_turns", neo_link_llm_clamp_turns(cfg->context_turns));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool neo_link_llm_is_ready(void)
{
    neo_link_llm_config_t cfg;
    if (neo_link_llm_load(&cfg) != ESP_OK) {
        return false;
    }
    return cfg.enabled && cfg.base_url[0] != '\0' && cfg.model[0] != '\0';
}

void neo_link_llm_status_json(cJSON *root)
{
    if (!root) {
        return;
    }
    neo_link_llm_config_t cfg;
    neo_link_llm_load(&cfg);
    cJSON_AddBoolToObject(root, "llm_enabled", cfg.enabled);
    cJSON_AddBoolToObject(root, "llm_ready", neo_link_llm_is_ready());
    cJSON_AddStringToObject(root, "llm_model", cfg.model);
    cJSON_AddStringToObject(root, "llm_base_url", cfg.base_url);
    cJSON_AddBoolToObject(root, "llm_has_key", cfg.api_key[0] != '\0');
    cJSON_AddNumberToObject(root, "llm_max_tokens", cfg.max_tokens);
    cJSON_AddNumberToObject(root, "llm_max_rpm", cfg.max_rpm);
    cJSON_AddNumberToObject(root, "llm_context_turns", cfg.context_turns);
    cJSON_AddNumberToObject(root, "llm_context_stored", s_ctx_count);
    cJSON_AddBoolToObject(root, "alphaword_coupled", false);
    if (s_last_error[0]) {
        cJSON_AddStringToObject(root, "llm_last_error", s_last_error);
    }
}

static void neo_link_llm_trim_trailing_slash(char *url)
{
    if (!url) {
        return;
    }
    size_t n = strlen(url);
    while (n > 0 && url[n - 1] == '/') {
        url[--n] = '\0';
    }
}

static esp_err_t neo_link_llm_build_chat_url(const neo_link_llm_config_t *cfg, char *out, size_t out_size)
{
    if (!cfg || !out || out_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    char base[NEO_LINK_LLM_URL_MAX];
    strlcpy(base, cfg->base_url, sizeof(base));
    neo_link_llm_trim_trailing_slash(base);
    if (base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    /* Accept either .../v1 or a full .../chat/completions URL. */
    if (strstr(base, "/chat/completions") != NULL) {
        strlcpy(out, base, out_size);
        return ESP_OK;
    }
    if (snprintf(out, out_size, "%s/chat/completions", base) >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void neo_link_llm_model_lower(const char *model, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t i = 0;
    if (model) {
        for (; model[i] && i + 1 < out_size; i++) {
            char c = model[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            out[i] = c;
        }
    }
    out[i] = '\0';
}

typedef struct {
    bool reasoning_family;   /* gpt-5 / o-series: modern OpenAI chat quirks */
    bool use_completion_tokens;
    bool allow_temperature;
    bool send_reasoning_effort;
    uint16_t completion_budget; /* API cap; may exceed UI max_tokens for reasoning headroom */
} neo_link_llm_model_profile_t;

static void neo_link_llm_model_profile(const char *model, uint16_t ui_tokens,
                                       neo_link_llm_model_profile_t *out)
{
    memset(out, 0, sizeof(*out));
    out->use_completion_tokens = false;
    out->allow_temperature = true;
    out->send_reasoning_effort = false;
    out->completion_budget = neo_link_llm_clamp_tokens(ui_tokens);

    char m[NEO_LINK_LLM_MODEL_MAX];
    neo_link_llm_model_lower(model, m, sizeof(m));

    /* Match aliases / dated snapshots: gpt-5-nano-2025-…, azure deploy names, etc. */
    const bool gpt5 = strstr(m, "gpt-5") != NULL;
    const bool o_series =
        (m[0] == 'o' && m[1] >= '1' && m[1] <= '9') ||
        strstr(m, "/o1") != NULL || strstr(m, "/o3") != NULL || strstr(m, "/o4") != NULL ||
        strstr(m, "o1-") != NULL || strstr(m, "o3-") != NULL || strstr(m, "o4-") != NULL ||
        strcmp(m, "o1") == 0 || strcmp(m, "o3") == 0 || strcmp(m, "o4") == 0;

    if (gpt5 || o_series) {
        out->reasoning_family = true;
        out->use_completion_tokens = true;
        out->allow_temperature = false;
        out->send_reasoning_effort = gpt5; /* gpt-5* accepts reasoning_effort; keep o1 conservative */
        /* Hidden reasoning tokens share this budget — 450 is often all burned with empty text. */
        uint32_t budget = (uint32_t)out->completion_budget * 4u;
        if (budget < 2048u) {
            budget = 2048u;
        }
        if (budget > 4096u) {
            budget = 4096u;
        }
        out->completion_budget = (uint16_t)budget;
    }
}

static bool neo_link_llm_extract_text_field(const cJSON *node, char *out, size_t out_size)
{
    if (!node || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (cJSON_IsString(node) && node->valuestring && node->valuestring[0]) {
        strlcpy(out, node->valuestring, out_size);
        return true;
    }
    /* Chat Completions content can be an array of parts: [{type:text,text:"…"}]. */
    if (cJSON_IsArray(node)) {
        size_t j = 0;
        const cJSON *part = NULL;
        cJSON_ArrayForEach(part, node) {
            const cJSON *t = cJSON_GetObjectItem(part, "text");
            if (!cJSON_IsString(t) || !t->valuestring) {
                continue;
            }
            for (size_t i = 0; t->valuestring[i] && j + 1 < out_size; i++) {
                out[j++] = t->valuestring[i];
            }
        }
        out[j] = '\0';
        return j > 0;
    }
    return false;
}

static char *neo_link_llm_build_body(const neo_link_llm_config_t *cfg, const char *prompt)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    neo_link_llm_model_profile_t prof;
    neo_link_llm_model_profile(cfg->model, cfg->max_tokens, &prof);

    cJSON_AddStringToObject(root, "model", cfg->model);
    if (prof.use_completion_tokens) {
        cJSON_AddNumberToObject(root, "max_completion_tokens", prof.completion_budget);
    } else {
        cJSON_AddNumberToObject(root, "max_tokens", prof.completion_budget);
    }
    if (prof.allow_temperature) {
        cJSON_AddNumberToObject(root, "temperature", 0.4);
    }
    if (prof.send_reasoning_effort) {
        /* Prefer visible Neo LCD text over long hidden reasoning. */
        cJSON_AddStringToObject(root, "reasoning_effort", "minimal");
    }

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (!messages) {
        cJSON_Delete(root);
        return NULL;
    }
    if (cfg->system[0]) {
        cJSON *sys = cJSON_CreateObject();
        if (!sys) {
            cJSON_Delete(root);
            return NULL;
        }
        /* system is widely accepted; developer is Responses-API oriented. */
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", cfg->system);
        cJSON_AddItemToArray(messages, sys);
    }
    uint8_t turns = neo_link_llm_clamp_turns(cfg->context_turns);
    uint8_t start = 0;
    if (s_ctx_count > turns) {
        start = (uint8_t)(s_ctx_count - turns);
    }
    for (uint8_t i = start; i < s_ctx_count; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON *a = cJSON_CreateObject();
        if (!u || !a) {
            cJSON_Delete(u);
            cJSON_Delete(a);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", s_ctx_user[i]);
        cJSON_AddStringToObject(a, "role", "assistant");
        cJSON_AddStringToObject(a, "content", s_ctx_asst[i]);
        cJSON_AddItemToArray(messages, u);
        cJSON_AddItemToArray(messages, a);
    }
    cJSON *user = cJSON_CreateObject();
    if (!user) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", prompt);
    cJSON_AddItemToArray(messages, user);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t neo_link_llm_parse_content(const char *json, char *out, size_t out_size, char *err, size_t err_size)
{
    if (!json || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, err_size, "invalid JSON response");
        return ESP_FAIL;
    }
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(error)) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            snprintf(err, err_size, "%s", msg->valuestring);
        } else {
            snprintf(err, err_size, "API error");
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) < 1) {
        snprintf(err, err_size, "no choices in API response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
    const char *finish_s = (cJSON_IsString(finish) && finish->valuestring) ? finish->valuestring : "";

    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    cJSON *refusal = message ? cJSON_GetObjectItem(message, "refusal") : NULL;
    if (cJSON_IsString(refusal) && refusal->valuestring && refusal->valuestring[0]) {
        snprintf(err, err_size, "model refused: %s", refusal->valuestring);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    char *raw = malloc(NEO_LINK_REPLY_MAX + 1);
    if (!raw) {
        cJSON_Delete(root);
        snprintf(err, err_size, "OOM parsing reply");
        return ESP_ERR_NO_MEM;
    }
    raw[0] = '\0';
    bool got = false;
    if (message) {
        got = neo_link_llm_extract_text_field(cJSON_GetObjectItem(message, "content"), raw,
                                              NEO_LINK_REPLY_MAX + 1);
    }
    if (!got) {
        got = neo_link_llm_extract_text_field(cJSON_GetObjectItem(choice0, "text"), raw,
                                              NEO_LINK_REPLY_MAX + 1);
    }
    if (!got) {
        /* Some gateways put the reply on delta (streaming leftover) or message.reasoning. */
        cJSON *delta = cJSON_GetObjectItem(choice0, "delta");
        if (delta) {
            got = neo_link_llm_extract_text_field(cJSON_GetObjectItem(delta, "content"), raw,
                                                  NEO_LINK_REPLY_MAX + 1);
        }
    }
    if (!got || raw[0] == '\0') {
        if (strcmp(finish_s, "length") == 0) {
            snprintf(err, err_size,
                     "Empty reply (finish=length). Reasoning used the token budget — "
                     "raise Max tokens or use gpt-4.1-mini / gpt-4o-mini.");
        } else {
            snprintf(err, err_size, "empty content (finish=%s)", finish_s[0] ? finish_s : "?");
        }
        free(raw);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Keep newlines for Neo Link scroll view; collapse tabs only. */
    size_t j = 0;
    for (size_t i = 0; raw[i] && j + 1 < out_size && j < NEO_LINK_REPLY_MAX; i++) {
        char c = raw[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\t') {
            c = ' ';
        }
        out[j++] = c;
    }
    out[j] = '\0';
    free(raw);
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\n')) {
        out[--j] = '\0';
    }
    cJSON_Delete(root);
    if (out[0] == '\0') {
        snprintf(err, err_size, "blank reply");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t neo_link_llm_chat(const char *prompt, char *out, size_t out_size, char *err, size_t err_size)
{
    if (!prompt || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!err || err_size == 0) {
        static char discard[4];
        err = discard;
        err_size = sizeof(discard);
    }
    err[0] = '\0';

    neo_link_llm_config_t cfg;
    esp_err_t load_err = neo_link_llm_load(&cfg);
    if (load_err != ESP_OK) {
        snprintf(err, err_size, "config load failed");
        return load_err;
    }
    if (!cfg.enabled || cfg.base_url[0] == '\0' || cfg.model[0] == '\0') {
        snprintf(err, err_size, "LLM not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_is_connected()) {
        snprintf(err, err_size, "Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!neo_link_llm_rate_check(cfg.max_rpm, err, err_size)) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[NEO_LINK_LLM_URL_MAX + 32];
    esp_err_t errc = neo_link_llm_build_chat_url(&cfg, url, sizeof(url));
    if (errc != ESP_OK) {
        snprintf(err, err_size, "bad base_url");
        return errc;
    }

    char *body = neo_link_llm_build_body(&cfg, prompt);
    if (!body) {
        snprintf(err, err_size, "oom body");
        return ESP_ERR_NO_MEM;
    }

    neo_link_llm_buf_t resp = {0};
    ESP_LOGI(TAG, "HTTPS to LLM (free=%u largest_internal=%u spiram_free=%u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = NEO_LINK_LLM_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    if (strncmp(url, "http://", 7) == 0) {
        http_cfg.crt_bundle_attach = NULL;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(body);
        snprintf(err, err_size,
                 "HTTP client init failed (out of memory). free=%u DRAM largest=%u PSRAM=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        neo_link_llm_set_last_error(err);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (cfg.api_key[0]) {
        char auth[NEO_LINK_LLM_KEY_MAX + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    } else if (strncmp(url, "https://", 8) == 0) {
        ESP_LOGW(TAG, "LLM HTTPS with empty API key");
    }

    esp_err_t ret = esp_http_client_open(client, (int)strlen(body));
    if (ret != ESP_OK) {
        neo_link_llm_format_connect_error(ret, err, err_size);
        ESP_LOGE(TAG, "LLM connect: %s", err);
        log_buffer_appendf("neo_link: %s", err);
        neo_link_llm_set_last_error(err);
        esp_http_client_cleanup(client);
        free(body);
        return ret;
    }
    int written = esp_http_client_write(client, body, (int)strlen(body));
    free(body);
    if (written < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(err, err_size, "HTTPS write failed after connect (server rejected or link dropped)");
        neo_link_llm_set_last_error(err);
        return ESP_FAIL;
    }

    (void)esp_http_client_fetch_headers(client);
    char tmp[512];
    while (1) {
        int r = esp_http_client_read(client, tmp, sizeof(tmp));
        if (r <= 0) {
            break;
        }
        size_t need = resp.len + (size_t)r + 1;
        if (need > NEO_LINK_LLM_RESP_MAX) {
            break;
        }
        if (need > resp.cap) {
            size_t ncap = resp.cap ? resp.cap * 2 : 1024;
            while (ncap < need) {
                ncap *= 2;
            }
            if (ncap > NEO_LINK_LLM_RESP_MAX) {
                ncap = NEO_LINK_LLM_RESP_MAX;
            }
            char *nbuf = realloc(resp.data, ncap);
            if (!nbuf) {
                break;
            }
            resp.data = nbuf;
            resp.cap = ncap;
        }
        size_t copy = (size_t)r;
        if (resp.len + copy + 1 > resp.cap) {
            copy = resp.cap - resp.len - 1;
        }
        memcpy(resp.data + resp.len, tmp, copy);
        resp.len += copy;
        resp.data[resp.len] = '\0';
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!resp.data || resp.len == 0) {
        free(resp.data);
        snprintf(err, err_size, "empty HTTP body (status %d)", status);
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        neo_link_llm_parse_content(resp.data, out, out_size, err, err_size);
        if (err && err[0] == '\0') {
            if (status == 401) {
                snprintf(err, err_size, "HTTP 401 unauthorized — check API key");
            } else if (status == 404) {
                snprintf(err, err_size, "HTTP 404 — check base URL ends with /v1 and model name");
            } else if (status == 429) {
                snprintf(err, err_size, "HTTP 429 rate limited by provider");
            } else {
                snprintf(err, err_size, "HTTP %d from LLM API", status);
            }
        }
        neo_link_llm_set_last_error(err);
        free(resp.data);
        return ESP_FAIL;
    }

    ret = neo_link_llm_parse_content(resp.data, out, out_size, err, err_size);
    free(resp.data);
    if (ret != ESP_OK) {
        neo_link_llm_set_last_error(err);
    } else {
        neo_link_llm_rate_commit();
        neo_link_llm_set_last_error(NULL);
    }
    return ret;
}

typedef struct {
    char *prompt;
} neo_link_llm_job_t;

static void neo_link_llm_chat_task(void *arg)
{
    neo_link_llm_job_t *job = arg;
    char *reply = malloc(NEO_LINK_REPLY_MAX + 1);
    char err[NEO_LINK_LLM_ERR_MAX];
    if (!reply) {
        ESP_LOGW(TAG, "LLM task OOM");
        if (job) {
            free(job->prompt);
            free(job);
        }
        if (s_busy) {
            xSemaphoreGive(s_busy);
        }
        vTaskDelete(NULL);
        return;
    }
    reply[0] = '\0';
    err[0] = '\0';

    if (job && job->prompt) {
        esp_err_t ret = ESP_ERR_INVALID_STATE;
        if (neo_link_llm_is_ready()) {
            log_buffer_appendf("neo_link: LLM request (%u chars)", (unsigned)strlen(job->prompt));
            ret = neo_link_llm_chat(job->prompt, reply, NEO_LINK_REPLY_MAX + 1, err, sizeof(err));
        } else {
            snprintf(err, sizeof(err), "LLM not ready (enable + base URL + model; API key for OpenAI)");
        }
        if (ret != ESP_OK) {
            if (err[0]) {
                neo_link_llm_set_last_error(err);
                ESP_LOGW(TAG, "LLM failed: %s — falling back to echo", err);
                log_buffer_appendf("neo_link: LLM fail %s", err);
            }
            snprintf(reply, NEO_LINK_REPLY_MAX + 1, "Echo: %s", job->prompt);
        } else {
            s_last_error[0] = '\0';
            neo_link_text_strip_markup(reply);
            char *plain = malloc(NEO_LINK_REPLY_MAX + 1);
            if (plain) {
                neo_link_text_to_mailbox(reply, plain, NEO_LINK_REPLY_MAX + 1);
                strlcpy(reply, plain, NEO_LINK_REPLY_MAX + 1);
                free(plain);
            }
            neo_link_llm_remember_turn(job->prompt, reply);
            log_buffer_appendf("neo_link: LLM reply (%u chars)", (unsigned)strlen(reply));
            ESP_LOGI(TAG, "LLM reply: %s", reply);
        }
        neo_link_transport_set_reply(reply);
        neo_link_mailbox_deliver(reply, true);
        free(job->prompt);
        free(job);
    }

    free(reply);
    if (s_busy) {
        xSemaphoreGive(s_busy);
    }
    vTaskDelete(NULL);
}

void neo_link_llm_handle_chat_async(const char *prompt)
{
    if (!prompt || prompt[0] == '\0') {
        return;
    }
    if (!s_busy) {
        s_busy = xSemaphoreCreateBinary();
        if (s_busy) {
            xSemaphoreGive(s_busy);
        }
    }
    if (s_busy && xSemaphoreTake(s_busy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "LLM busy — dropping prompt");
        log_buffer_appendf("neo_link: LLM busy, drop");
        return;
    }

    neo_link_llm_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        if (s_busy) {
            xSemaphoreGive(s_busy);
        }
        return;
    }
    job->prompt = strdup(prompt);
    if (!job->prompt) {
        free(job);
        if (s_busy) {
            xSemaphoreGive(s_busy);
        }
        return;
    }
    if (xTaskCreate(neo_link_llm_chat_task, "neo_link_llm", NEO_LINK_LLM_TASK_STACK, job, 5, NULL) !=
        pdPASS) {
        free(job->prompt);
        free(job);
        if (s_busy) {
            xSemaphoreGive(s_busy);
        }
        ESP_LOGW(TAG, "failed to start LLM task");
    }
}

static bool neo_link_llm_req_auth(httpd_req_t *req)
{
    char auth_header[160];
    const char *prefix = "Bearer ";
    return httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK &&
           strncmp(auth_header, prefix, strlen(prefix)) == 0 &&
           auth_check_token(auth_header + strlen(prefix));
}

static esp_err_t neo_link_llm_get_handler(httpd_req_t *req)
{
    if (!neo_link_llm_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    neo_link_llm_config_t cfg;
    neo_link_llm_load(&cfg);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddStringToObject(root, "base_url", cfg.base_url);
    cJSON_AddStringToObject(root, "model", cfg.model);
    cJSON_AddStringToObject(root, "system", cfg.system);
    cJSON_AddNumberToObject(root, "max_tokens", cfg.max_tokens);
    cJSON_AddNumberToObject(root, "max_rpm", cfg.max_rpm);
    cJSON_AddNumberToObject(root, "context_turns", cfg.context_turns);
    cJSON_AddNumberToObject(root, "context_stored", s_ctx_count);
    cJSON_AddBoolToObject(root, "has_api_key", cfg.api_key[0] != '\0');
    cJSON_AddBoolToObject(root, "alphaword_coupled", false);
    /* Never return raw key; allow optional masked hint */
    if (cfg.api_key[0]) {
        char hint[12];
        size_t n = strlen(cfg.api_key);
        snprintf(hint, sizeof(hint), "...%s", n > 4 ? cfg.api_key + n - 4 : "****");
        cJSON_AddStringToObject(root, "api_key_hint", hint);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t neo_link_llm_put_handler(httpd_req_t *req)
{
    if (!neo_link_llm_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char *body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_OK;
    }

    neo_link_llm_config_t cfg;
    neo_link_llm_load(&cfg);
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en)) {
        cfg.enabled = cJSON_IsTrue(en);
    }
    cJSON *url = cJSON_GetObjectItem(root, "base_url");
    if (cJSON_IsString(url) && url->valuestring) {
        strlcpy(cfg.base_url, url->valuestring, sizeof(cfg.base_url));
    }
    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model) && model->valuestring) {
        strlcpy(cfg.model, model->valuestring, sizeof(cfg.model));
    }
    cJSON *system = cJSON_GetObjectItem(root, "system");
    if (cJSON_IsString(system) && system->valuestring) {
        strlcpy(cfg.system, system->valuestring, sizeof(cfg.system));
    }
    cJSON *key = cJSON_GetObjectItem(root, "api_key");
    if (cJSON_IsString(key) && key->valuestring) {
        /* Empty string clears key; omit field to leave unchanged. */
        strlcpy(cfg.api_key, key->valuestring, sizeof(cfg.api_key));
    }
    cJSON *tok = cJSON_GetObjectItem(root, "max_tokens");
    if (cJSON_IsNumber(tok)) {
        cfg.max_tokens = neo_link_llm_clamp_tokens(tok->valueint);
    }
    cJSON *rpm = cJSON_GetObjectItem(root, "max_rpm");
    if (cJSON_IsNumber(rpm)) {
        cfg.max_rpm = neo_link_llm_clamp_rpm(rpm->valueint);
    }
    cJSON *turns = cJSON_GetObjectItem(root, "context_turns");
    if (cJSON_IsNumber(turns)) {
        cfg.context_turns = neo_link_llm_clamp_turns(turns->valueint);
    }
    cJSON *clear_ctx = cJSON_GetObjectItem(root, "clear_context");
    if (cJSON_IsTrue(clear_ctx)) {
        neo_link_llm_clear_context();
    }
    cJSON_Delete(root);

    esp_err_t err = neo_link_llm_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    log_buffer_appendf("neo_link: LLM config saved enabled=%d model=%s", cfg.enabled ? 1 : 0, cfg.model);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t neo_link_llm_test_handler(httpd_req_t *req)
{
    if (!neo_link_llm_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char prompt[256] = "Reply with exactly: Neo Link OK";
    if (req->content_len > 0 && req->content_len < (int)sizeof(prompt)) {
        int got = httpd_req_recv(req, prompt, req->content_len);
        if (got > 0) {
            prompt[got] = '\0';
        }
    }
    char *reply = malloc(NEO_LINK_REPLY_MAX + 1);
    if (!reply) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    char err[NEO_LINK_LLM_ERR_MAX];
    esp_err_t ret = neo_link_llm_chat(prompt, reply, NEO_LINK_REPLY_MAX + 1, err, sizeof(err));
    if (ret != ESP_OK && err[0]) {
        neo_link_llm_set_last_error(err);
    } else if (ret == ESP_OK) {
        neo_link_llm_set_last_error(NULL);
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(reply);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", ret == ESP_OK);
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(root, "reply", reply);
    } else {
        cJSON_AddStringToObject(root, "error", err[0] ? err : esp_err_to_name(ret));
        if (s_last_error[0]) {
            cJSON_AddStringToObject(root, "detail", s_last_error);
        }
    }
    free(reply);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t neo_link_llm_clear_handler(httpd_req_t *req)
{
    if (!neo_link_llm_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* Drain optional body */
    if (req->content_len > 0) {
        char dump[64];
        while (httpd_req_recv(req, dump, sizeof(dump)) > 0) {
        }
    }
    neo_link_llm_clear_context();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"context_stored\":0}");
    return ESP_OK;
}

esp_err_t neo_link_llm_web_register(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t get_uri = {
        .uri = "/api/v1/link/llm",
        .method = HTTP_GET,
        .handler = neo_link_llm_get_handler,
    };
    httpd_uri_t put_uri = {
        .uri = "/api/v1/link/llm",
        .method = HTTP_PUT,
        .handler = neo_link_llm_put_handler,
    };
    httpd_uri_t test_uri = {
        .uri = "/api/v1/link/llm/test",
        .method = HTTP_POST,
        .handler = neo_link_llm_test_handler,
    };
    httpd_uri_t clear_uri = {
        .uri = "/api/v1/link/llm/clear-context",
        .method = HTTP_POST,
        .handler = neo_link_llm_clear_handler,
    };
    esp_err_t r = httpd_register_uri_handler(server, &get_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &put_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &test_uri);
    if (r != ESP_OK) {
        return r;
    }
    return httpd_register_uri_handler(server, &clear_uri);
}
