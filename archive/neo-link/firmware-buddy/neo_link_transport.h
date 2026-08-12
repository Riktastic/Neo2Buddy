/**
 * @file neo_link_transport.h
 * @brief ESP32-side Host Link Transport (HLT) decoder and stub API.
 *
 * Gated by CONFIG_BUDDY_NEO_LINK. See neo-link/ in the repo root.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

/** Feed decoded HID text; returns true if a complete frame was accepted. */
bool neo_link_transport_feed_text(const char *text, size_t len);

/** Register experimental /api/v1/link handlers. */
esp_err_t neo_link_web_register(httpd_handle_t server);

/** Last decoded prompt (NUL-terminated). */
const char *neo_link_transport_last_prompt(void);

/** Stub reply text (echo or future LLM output). */
const char *neo_link_transport_last_reply(void);

void neo_link_transport_set_reply(const char *reply);

bool neo_link_transport_in_frame(void);
