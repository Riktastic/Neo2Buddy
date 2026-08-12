/**
 * @file stock_applets.h
 * @brief Bundled stock SmartApplets for the buddy App Store.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

size_t stock_applets_count(void);
esp_err_t stock_applets_install(const char *slug, bool replace);
/** Write a normalized front|back deck (raw ASCII) into Flash Cards file 1. */
esp_err_t stock_applets_flash_deck_write(const uint8_t *data, size_t length);
esp_err_t stock_applets_web_register(httpd_handle_t server);
