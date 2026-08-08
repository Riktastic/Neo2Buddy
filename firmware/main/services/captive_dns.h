/**
 * @file captive_dns.h
 * @brief Minimal captive-portal DNS responder API.
 */

#pragma once

#include "esp_err.h"

esp_err_t captive_dns_start(void);
esp_err_t captive_dns_stop(void);
