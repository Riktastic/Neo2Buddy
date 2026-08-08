#include "unity.h"
#include "auth.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <string.h>

static void nvs_reset_auth(void)
{
    nvs_handle_t h;
    if (nvs_open("auth", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    auth_test_reset();
    auth_init();
}

void setUp(void)
{
    nvs_reset_auth();
}

void tearDown(void)
{
}

TEST_CASE("auth login issues valid token", "[auth]")
{
    char token[65];
    TEST_ASSERT_EQUAL(ESP_OK, auth_login("neo2buddy", token, sizeof(token)));
    TEST_ASSERT_EQUAL(64, strlen(token));
    TEST_ASSERT_TRUE(auth_check_token(token));
}

TEST_CASE("auth rejects wrong password", "[auth]")
{
    char token[65];
    TEST_ASSERT_EQUAL(ESP_FAIL, auth_login("wrong", token, sizeof(token)));
    TEST_ASSERT_FALSE(auth_check_token(token));
}

TEST_CASE("auth logout invalidates token", "[auth]")
{
    char token[65];
    TEST_ASSERT_EQUAL(ESP_OK, auth_login("neo2buddy", token, sizeof(token)));
    TEST_ASSERT_EQUAL(ESP_OK, auth_logout());
    TEST_ASSERT_FALSE(auth_check_token(token));
}

TEST_CASE("auth refresh rotates token", "[auth]")
{
    char token[65];
    char refreshed[65];
    TEST_ASSERT_EQUAL(ESP_OK, auth_login("neo2buddy", token, sizeof(token)));
    TEST_ASSERT_EQUAL(ESP_OK, auth_refresh(token, refreshed, sizeof(refreshed)));
    TEST_ASSERT_FALSE(auth_check_token(token));
    TEST_ASSERT_TRUE(auth_check_token(refreshed));
}

TEST_CASE("auth set password persists", "[auth]")
{
    char token[65];
    TEST_ASSERT_EQUAL(ESP_OK, auth_set_password("my-secret"));
    TEST_ASSERT_EQUAL(ESP_FAIL, auth_login("neo2buddy", token, sizeof(token)));
    TEST_ASSERT_EQUAL(ESP_OK, auth_login("my-secret", token, sizeof(token)));
    TEST_ASSERT_TRUE(auth_check_token(token));
}

TEST_CASE("auth change password requires current", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, auth_change_password("wrong", "new-secret1"));
    TEST_ASSERT_EQUAL(ESP_OK, auth_change_password("neo2buddy", "new-secret1"));
    char token[65];
    TEST_ASSERT_EQUAL(ESP_FAIL, auth_login("neo2buddy", token, sizeof(token)));
    TEST_ASSERT_EQUAL(ESP_OK, auth_login("new-secret1", token, sizeof(token)));
}

TEST_CASE("auth change password rejects short new password", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, auth_change_password("neo2buddy", "short"));
}

TEST_CASE("auth rate limits brute force", "[auth]")
{
    char token[65];
    for (int i = 0; i < 6; ++i) {
        (void)auth_login("bad", token, sizeof(token));
    }
    TEST_ASSERT_TRUE(auth_login_rate_limited());
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, auth_login("neo2buddy", token, sizeof(token)));
}
