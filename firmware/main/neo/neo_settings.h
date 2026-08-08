/**
 * @file neo_settings.h
 * @brief SmartApplet settings blobs (GET_SETTINGS / SET_SETTINGS).
 *
 * Settings are typed binary records (label, range, option list, password, …)
 * returned in batches from the applet. We parse them to JSON for the portal.
 * Types mirror NeoTools neo_settings.py constants (0x0102 range, 0x0103 option, …).
 */

#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

esp_err_t neo_settings_get_json(uint16_t applet_id, uint32_t flags, cJSON *out_array);
esp_err_t neo_settings_get_merged_json(uint16_t applet_id, cJSON *out_array);
esp_err_t neo_settings_set_by_ident(uint16_t applet_id, uint16_t ident, cJSON *values);
esp_err_t neo_settings_set_json(uint16_t applet_id, cJSON *items);
