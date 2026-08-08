#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_api_register(httpd_handle_t server);
/** Record boot time so network-setting reboots can be debounced. */
void web_api_note_boot_time(void);
