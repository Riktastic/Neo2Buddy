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

bool neo_import_neo_raw_is_empty(const uint8_t *raw, size_t raw_len)
{
    if (!raw || raw_len == 0) {
        return true;
    }
    for (size_t i = 0; i < raw_len; i++) {
        uint8_t b = raw[i];
        /* AlphaWord pads unused space with 0xa7 (NeoTools); 0xa4 also unused. */
        if (b != 0xa7 && b != 0xa4 && b != 0x00) {
            return false;
        }
    }
    return true;
}
