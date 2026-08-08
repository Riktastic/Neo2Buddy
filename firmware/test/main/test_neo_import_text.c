#include "unity.h"
#include "neo_import.h"
#include <string.h>

TEST_CASE("blank text detection", "[import]")
{
    TEST_ASSERT_TRUE(neo_import_text_is_blank(NULL, 0));
    TEST_ASSERT_TRUE(neo_import_text_is_blank("", 0));
    TEST_ASSERT_TRUE(neo_import_text_is_blank("   \t\r\n  ", 8));
    TEST_ASSERT_FALSE(neo_import_text_is_blank("a", 1));
    TEST_ASSERT_FALSE(neo_import_text_is_blank("  hello  ", 9));
    TEST_ASSERT_FALSE(neo_import_text_is_blank("\nline\n", 6));
}
