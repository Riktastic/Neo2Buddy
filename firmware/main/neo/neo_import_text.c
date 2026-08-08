/**
 * @file neo_import_text.c
 * @brief Pure helpers for backup content policy (no filesystem I/O).
 */

#include "neo_import.h"

bool neo_import_text_is_blank(const char *text, size_t text_len)
{
    if (!text || text_len == 0) {
        return true;
    }
    for (size_t i = 0; i < text_len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}
