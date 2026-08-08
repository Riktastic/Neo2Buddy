/**
 * @file auth.h
 * @brief Portal password login and bearer-token session API.
 *
 * The web UI and UART console share one password stored in NVS. Successful
 * login returns a random bearer token; HTTP handlers call auth_check_token().
 * Tokens can be rotated via auth_refresh(). Failed logins are rate-limited to
 * slow brute-force attempts on the local AP.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Load password/token from NVS (call once at boot). */
esp_err_t auth_init(void);

/**
 * Validate password and write a new bearer token into out_token.
 * Returns ESP_ERR_INVALID_ARG on wrong password or when rate-limited.
 */
esp_err_t auth_login(const char *password, char *out_token, size_t out_token_size);

/** Clear the active session token in NVS. */
esp_err_t auth_logout(void);

/** True when token matches the stored session (not expired). */
bool auth_check_token(const char *token);

/** Issue a new token for a valid existing token (rotation). */
esp_err_t auth_refresh(const char *token, char *out_token, size_t out_token_size);

/** True while login attempts are temporarily blocked after failures. */
bool auth_login_rate_limited(void);

/** Token expiry as Unix epoch seconds (0 if none / not logged in). */
uint64_t auth_get_token_expiry(void);

/** Persist a new portal password (plaintext in NVS; change default on first setup). */
esp_err_t auth_set_password(const char *password);

/**
 * Change portal password after verifying current_password.
 * New password must be 8..63 characters.
 */
esp_err_t auth_change_password(const char *current_password, const char *new_password);

/** Validate portal password without issuing a web token (UART console login). */
bool auth_check_password(const char *password);

#ifdef CONFIG_BUDDY_TEST_BUILD
/** Reset in-memory auth state between unit tests. */
void auth_test_reset(void);
#endif
