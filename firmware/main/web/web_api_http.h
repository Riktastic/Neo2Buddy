#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t web_api_register(httpd_handle_t server);
/** Record boot time so network-setting reboots can be debounced. */
void web_api_note_boot_time(void);

/**
 * URI matcher for httpd: mid-path `*` matches one path segment; a trailing `*`
 * matches the remainder (ESP-IDF style). Use instead of httpd_uri_match_wildcard,
 * which treats mid-path `*` as a literal character.
 */
bool web_api_uri_match(const char *template, const char *uri, size_t uri_len);
