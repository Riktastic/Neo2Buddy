#include "unity.h"
#include "neo_message.h"

TEST_CASE("neo_message builds checksum", "[neo]")
{
    neo_message_t msg;
    const uint32_t args[][3] = { {0x1234, 1, 2}, {0, 0, 0} };
    neo_message_init(&msg, 0x10, args);
    TEST_ASSERT_EQUAL(0x10, neo_message_command(&msg));
    TEST_ASSERT_TRUE(neo_message_checksum_is_valid(&msg));
    TEST_ASSERT_EQUAL(0x1234, neo_message_argument(&msg, 1, 2));
}

TEST_CASE("neo_message detects corrupt checksum", "[neo]")
{
    neo_message_t msg;
    neo_message_init(&msg, 0x20, NULL);
    msg.data[7] ^= 0xFF;
    TEST_ASSERT_FALSE(neo_message_checksum_is_valid(&msg));
}

TEST_CASE("neo_message maps known errors", "[neo]")
{
    TEST_ASSERT_NOT_NULL(neo_message_error_string(NEO_ERROR_INVALID_APPLET));
    TEST_ASSERT_NOT_NULL(neo_message_error_string(NEO_ERROR_OUTOFMEMORY));
}

TEST_CASE("neo_message encodes file attributes request", "[neo]")
{
    neo_message_t msg;
    const uint32_t args[][3] = { {1, 4, 1}, {0xA000, 5, 2}, {0, 0, 0} };
    neo_message_init(&msg, NEO_REQUEST_GET_FILE_ATTRIBUTES, args);
    TEST_ASSERT_EQUAL(NEO_REQUEST_GET_FILE_ATTRIBUTES, neo_message_command(&msg));
    TEST_ASSERT_TRUE(neo_message_checksum_is_valid(&msg));
    TEST_ASSERT_EQUAL(1, neo_message_argument(&msg, 4, 1));
    TEST_ASSERT_EQUAL(0xA000, neo_message_argument(&msg, 5, 2));
}
