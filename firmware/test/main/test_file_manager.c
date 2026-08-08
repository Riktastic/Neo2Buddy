#include "unity.h"
#include "file_manager.h"
#include <string.h>

TEST_CASE("file_manager accepts safe names", "[files]")
{
    TEST_ASSERT_EQUAL(ESP_OK, file_manager_validate_name("notes.txt"));
    TEST_ASSERT_EQUAL(ESP_OK, file_manager_validate_name("draft-2026.txt"));
}

TEST_CASE("file_manager rejects unsafe names", "[files]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, file_manager_validate_name("../etc/passwd"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, file_manager_validate_name("sub/file.txt"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, file_manager_validate_name(".hidden"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, file_manager_validate_name(""));
}

TEST_CASE("file_manager resolve path stays in base", "[files]")
{
    char path[128];
    TEST_ASSERT_EQUAL(ESP_OK, file_manager_resolve_path("hello.txt", path, sizeof(path)));
    TEST_ASSERT_NOT_NULL(strstr(path, "hello.txt"));
}
