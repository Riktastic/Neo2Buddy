/**
 * @file auth.c
 * @brief Simple password + token-based auth backed by NVS.
 *
 * This lightweight auth implementation stores a password and a generated
 * bearer token in NVS. `auth_login` validates the provided password and
 * returns a random token; `auth_check_token` validates tokens against the
 * cached value (loaded from NVS on demand).
 *
 * Notes: the current design is intentionally simple for the local web UI.
 * Consider stronger password policies, rate-limiting, or integrating a
 * platform identity provider for production deployments.
 */

#include "auth.h"
#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <stdint.h>

#define AUTH_NAMESPACE "auth"
#define KEY_TOKEN "token"
#define KEY_PASSWORD "pw"
#define KEY_TOKEN_EXP "token_exp"

static char current_token[65];
static uint64_t current_token_exp = 0; /* epoch seconds */

/* Simple in-memory rate limiter for login attempts: counts attempts within
 * a sliding window. This is intentionally simple: good enough for local UI. */
#define LOGIN_WINDOW_SECONDS 60
#define LOGIN_MAX_ATTEMPTS 6
static int login_attempts = 0;
static uint64_t login_window_start = 0;

/* Load token from NVS into the module cache (called from init or on-demand). */
esp_err_t auth_init(void) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(AUTH_NAMESPACE, NVS_READONLY, &h);
    if (r != ESP_OK) return ESP_OK; // no stored data yet
    size_t sz = sizeof(current_token);
    if (nvs_get_str(h, KEY_TOKEN, current_token, &sz) != ESP_OK) {
        current_token[0] = '\0';
    }
    uint32_t exp = 0;
    if (nvs_get_u32(h, KEY_TOKEN_EXP, &exp) == ESP_OK) current_token_exp = exp;
    nvs_close(h);
    return ESP_OK;
}

/* Persist a generated token into NVS. */
static esp_err_t store_token(const char *token) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_str(h, KEY_TOKEN, token);
    if (r == ESP_OK) r = nvs_set_u32(h, KEY_TOKEN_EXP, (uint32_t)current_token_exp);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

/* Validate provided password, generate a random 32-byte hex token, store it
 * and return the token to the caller. The stored password defaults to
 * "neo2buddy" when no user-specified password is present. */
esp_err_t auth_login(const char *password, char *out_token, size_t out_token_size) {
    /* Rate-limit simple brute-force attempts */
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
    if (login_window_start == 0 || now - login_window_start > LOGIN_WINDOW_SECONDS) {
        login_window_start = now;
        login_attempts = 0;
    }
    if (login_attempts >= LOGIN_MAX_ATTEMPTS) return ESP_ERR_NO_MEM; /* signal rate limited */
    if (!password || !out_token) return ESP_ERR_INVALID_ARG;
    /* Read stored password if exists, otherwise default to "neo2buddy" */
    char stored_pw[64] = {0};
    nvs_handle_t h;
    if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(stored_pw);
        if (nvs_get_str(h, KEY_PASSWORD, stored_pw, &sz) != ESP_OK) {
            strcpy(stored_pw, "neo2buddy");
        }
        nvs_close(h);
    } else {
        strcpy(stored_pw, "neo2buddy");
    }

    /* Use constant-time comparison to avoid timing leaks */
    bool pw_ok = false;
    size_t lp = strlen(password);
    size_t ls = strlen(stored_pw);
    if (lp == ls) {
        /* constant-time memcmp */
        volatile unsigned char diff = 0;
        for (size_t i = 0; i < lp; ++i) diff |= password[i] ^ stored_pw[i];
        if (diff == 0) pw_ok = true;
    }
    if (!pw_ok) {
        login_attempts++;
        return ESP_FAIL;
    }

    /* Successful login: reset window */
    login_window_start = 0;
    login_attempts = 0;

    /* Generate token (hex of 32 bytes) */
    uint8_t tmp[32];
    for (int i = 0; i < 32; ++i) tmp[i] = (uint8_t)(esp_random() & 0xFF);
    char tok[65];
    for (int i = 0; i < 32; ++i) sprintf(&tok[i*2], "%02x", tmp[i]);
    tok[64] = '\0';

    /* Expiry must be set before store_token — NVS persists token_exp with the token. */
    current_token_exp = ((uint64_t)esp_timer_get_time() / 1000000ULL) + (15 * 60);
    esp_err_t r = store_token(tok);
    if (r == ESP_OK) {
        strncpy(current_token, tok, sizeof(current_token) - 1);
        current_token[sizeof(current_token) - 1] = '\0';
        strncpy(out_token, tok, out_token_size);
        if (out_token_size > 0) {
            out_token[out_token_size - 1] = '\0';
        }
        return ESP_OK;
    }
    current_token_exp = 0;
    return r;
}

/* Remove stored token from NVS and clear cache. */
esp_err_t auth_logout(void) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_erase_key(h, KEY_TOKEN);
    nvs_erase_key(h, KEY_TOKEN_EXP);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    current_token[0] = '\0';
    current_token_exp = 0;
    return r;
}

/* Check provided token against the cached value. Loads from NVS if needed. */
bool auth_check_token(const char *token) {
    if (!token || token[0] == '\0') return false;
    if (current_token[0] == '\0') {
        /* load from NVS */
        nvs_handle_t h;
        if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
        size_t sz = sizeof(current_token);
        if (nvs_get_str(h, KEY_TOKEN, current_token, &sz) != ESP_OK) {
            nvs_close(h);
            return false;
        }
        uint32_t exp = 0;
        if (nvs_get_u32(h, KEY_TOKEN_EXP, &exp) == ESP_OK) current_token_exp = exp;
        nvs_close(h);
    }
    if (strcmp(token, current_token) != 0) return false;
    /* Tokens without an expiry (legacy bug) are not accepted — force re-login. */
    if (current_token_exp == 0) return false;
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
    if (now > current_token_exp) return false; /* expired */
    return true;
}

/* Rotate a valid token and return a new token; keeps expiry window fresh. */
esp_err_t auth_refresh(const char *token, char *out_token, size_t out_token_size) {
    if (!token || !out_token) return ESP_ERR_INVALID_ARG;
    if (!auth_check_token(token)) return ESP_FAIL;
    /* generate new token */
    uint8_t tmp[32];
    for (int i = 0; i < 32; ++i) tmp[i] = (uint8_t)(esp_random() & 0xFF);
    char tok[65];
    for (int i = 0; i < 32; ++i) sprintf(&tok[i*2], "%02x", tmp[i]);
    tok[64] = '\0';
    current_token_exp = ((uint64_t)esp_timer_get_time() / 1000000ULL) + (15 * 60);
    esp_err_t r = store_token(tok);
    if (r == ESP_OK) {
        strncpy(current_token, tok, sizeof(current_token));
        strncpy(out_token, tok, out_token_size);
        return ESP_OK;
    }
    return r;
}

bool auth_login_rate_limited(void) {
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
    if (login_window_start == 0 || now - login_window_start > LOGIN_WINDOW_SECONDS) return false;
    return login_attempts >= LOGIN_MAX_ATTEMPTS;
}

uint64_t auth_get_token_expiry(void) {
    return current_token_exp;
}

esp_err_t auth_set_password(const char *password)
{
    if (!password || password[0] == '\0' || strlen(password) >= 64) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t r = nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        return r;
    }
    r = nvs_set_str(h, KEY_PASSWORD, password);
    if (r == ESP_OK) {
        r = nvs_commit(h);
    }
    nvs_close(h);
    return r;
}

esp_err_t auth_change_password(const char *current_password, const char *new_password)
{
    if (!current_password || !new_password) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t new_len = strlen(new_password);
    if (new_len < 8 || new_len >= 64) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (auth_login_rate_limited()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!auth_check_password(current_password)) {
        return ESP_ERR_INVALID_ARG;
    }
    return auth_set_password(new_password);
}

bool auth_check_password(const char *password)
{
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
    if (login_window_start == 0 || now - login_window_start > LOGIN_WINDOW_SECONDS) {
        login_window_start = now;
        login_attempts = 0;
    }
    if (login_attempts >= LOGIN_MAX_ATTEMPTS) {
        return false;
    }
    if (!password) {
        return false;
    }
    char stored_pw[64] = {0};
    nvs_handle_t h;
    if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(stored_pw);
        if (nvs_get_str(h, KEY_PASSWORD, stored_pw, &sz) != ESP_OK) {
            strcpy(stored_pw, "neo2buddy");
        }
        nvs_close(h);
    } else {
        strcpy(stored_pw, "neo2buddy");
    }
    size_t lp = strlen(password);
    size_t ls = strlen(stored_pw);
    if (lp != ls) {
        login_attempts++;
        return false;
    }
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < lp; ++i) {
        diff |= (unsigned char)password[i] ^ (unsigned char)stored_pw[i];
    }
    if (diff != 0) {
        login_attempts++;
        return false;
    }
    login_window_start = 0;
    login_attempts = 0;
    return true;
}

#ifdef CONFIG_BUDDY_TEST_BUILD
void auth_test_reset(void)
{
    login_attempts = 0;
    login_window_start = 0;
    current_token[0] = '\0';
    current_token_exp = 0;
}
#endif
