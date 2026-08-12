/**
 * @file flash_decks.h
 * @brief Buddy-local flashcard deck library (multi-set) + HTTP API.
 *
 * Decks live on SPIFFS as flat files fc_<id>.json. The active Neo deck is
 * still a single push of front|back lines into the Flash Cards applet.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t flash_decks_web_register(httpd_handle_t server);
