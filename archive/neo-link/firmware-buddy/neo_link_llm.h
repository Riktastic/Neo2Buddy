/**
 * @file neo_link_llm.h
 * @brief OpenAI-compatible chat client for Neo Link (separate from AlphaWord).
 *
 * Config in NVS namespace "neo_link". API keys never leave the buddy.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cJSON;

#define NEO_LINK_LLM_URL_MAX 256
#define NEO_LINK_LLM_KEY_MAX 192
#define NEO_LINK_LLM_MODEL_MAX 64
#define NEO_LINK_LLM_SYSTEM_MAX 256

typedef struct {
    bool enabled;
    char base_url[NEO_LINK_LLM_URL_MAX];
    char api_key[NEO_LINK_LLM_KEY_MAX];
    char model[NEO_LINK_LLM_MODEL_MAX];
    char system[NEO_LINK_LLM_SYSTEM_MAX];
    uint16_t max_tokens; /**< clamped 50..800 */
    uint8_t max_rpm;     /**< requests per minute, 1..30 */
    uint8_t context_turns; /**< prior user/assistant pairs, 0..2 */
} neo_link_llm_config_t;

void neo_link_llm_defaults(neo_link_llm_config_t *cfg);
esp_err_t neo_link_llm_load(neo_link_llm_config_t *cfg);
esp_err_t neo_link_llm_save(const neo_link_llm_config_t *cfg);

bool neo_link_llm_is_ready(void);

/** Clear in-memory conversation context (not NVS). */
void neo_link_llm_clear_context(void);

void neo_link_llm_handle_chat_async(const char *prompt);

esp_err_t neo_link_llm_chat(const char *prompt, char *out, size_t out_size, char *err, size_t err_size);

esp_err_t neo_link_llm_web_register(httpd_handle_t server);

void neo_link_llm_status_json(struct cJSON *root);
