/**
 * @file neo_link_text.c
 * @brief Plain ASCII mailbox encoding (NeoTools non-AlphaWord semantics).
 */

#include "neo_link_text.h"

#include <ctype.h>
#include <string.h>

size_t neo_link_text_to_mailbox(const char *utf8, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!utf8) {
        return 0;
    }

    size_t j = 0;
    for (size_t i = 0; utf8[i] != '\0' && j + 1 < out_cap; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out[j++] = '\n';
            continue;
        }
        if (c == '\t') {
            out[j++] = ' ';
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            continue;
        }
        if (c >= 0x80) {
            /* Skip continuation bytes; replace lead with '?' once. */
            if ((c & 0xc0) != 0x80) {
                out[j++] = '?';
            }
            continue;
        }
        out[j++] = (char)c;
    }
    while (j > 0 && out[j - 1] == ' ') {
        j--;
    }
    out[j] = '\0';
    return j;
}

void neo_link_text_strip_markup(char *s)
{
    if (!s) {
        return;
    }
    /* Remove common markdown markers that look bad on Neo LCD. */
    char *d = s;
    for (char *p = s; *p; p++) {
        if (*p == '*' || *p == '`' || *p == '#' || *p == '>' ) {
            continue;
        }
        if (*p == '[' || *p == ']') {
            continue;
        }
        *d++ = *p;
    }
    *d = '\0';

    /* Collapse repeated spaces */
    d = s;
    int space = 0;
    for (char *p = s; *p; p++) {
        if (*p == ' ') {
            if (space) {
                continue;
            }
            space = 1;
        } else {
            space = 0;
        }
        *d++ = *p;
    }
    *d = '\0';
}
