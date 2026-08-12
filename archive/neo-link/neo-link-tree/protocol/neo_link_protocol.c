/**
 * @file neo_link_protocol.c
 * @brief HLT framing codec — ESP32 buddy only (not linked into Neo applet).
 *
 * Neo-side rules: neo-link/docs/os3k-applet.md
 */

#include "neo_link_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    PARSE_IDLE,
    PARSE_PREFIX,
    PARSE_HEX,
    PARSE_SUFFIX_BAR,
    PARSE_SUFFIX_TILDE,
} neo_link_parse_state_t;

static neo_link_parse_state_t s_state;
static char s_hex[NEO_LINK_MAX_HEX + 1];
static size_t s_hex_len;
static size_t s_prefix_idx;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void neo_link_parser_reset(void)
{
    s_state = PARSE_IDLE;
    s_hex_len = 0;
    s_hex[0] = '\0';
    s_prefix_idx = 0;
}

bool neo_link_parse_hex_payload(const char *hex, size_t hex_len, neo_link_message_t *out)
{
    if (!out || hex_len < 2 || (hex_len % 2) != 0 || hex_len > NEO_LINK_MAX_HEX) {
        return false;
    }

#if defined(__m68k__) || defined(__mc68000__)
    static uint8_t s_parse_buf[NEO_LINK_MAX_PAYLOAD];
    uint8_t *buf = s_parse_buf;
#else
    uint8_t buf_local[NEO_LINK_MAX_PAYLOAD];
    uint8_t *buf = buf_local;
#endif
    size_t byte_len = hex_len / 2;
    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }

    memset(out, 0, sizeof(*out));
    if (byte_len == 0) {
        return false;
    }

    out->type = (neo_link_msg_type_t)buf[0];
    if (out->type == NEO_LINK_MSG_CHAT) {
        size_t text_len = byte_len - 1;
        if (text_len > NEO_LINK_MAX_PAYLOAD) {
            return false;
        }
        memcpy(out->text, buf + 1, text_len);
        out->text[text_len] = '\0';
        out->text_len = text_len;
        return true;
    }
    if (out->type == NEO_LINK_MSG_PING || out->type == NEO_LINK_MSG_ABORT) {
        return byte_len == 1;
    }
    return false;
}

bool neo_link_parser_feed(char ch, neo_link_message_t *out)
{
    static const char *prefix = NEO_LINK_FRAME_PREFIX;
    static const size_t prefix_len = sizeof(NEO_LINK_FRAME_PREFIX) - 1;

    if (!out) {
        return false;
    }

    switch (s_state) {
    case PARSE_IDLE:
        if (ch == prefix[0]) {
            s_state = PARSE_PREFIX;
            s_prefix_idx = 1;
        }
        return false;

    case PARSE_PREFIX:
        if (s_prefix_idx < prefix_len && ch == prefix[s_prefix_idx]) {
            s_prefix_idx++;
            if (s_prefix_idx == prefix_len) {
                s_state = PARSE_HEX;
                s_hex_len = 0;
            }
            return false;
        }
        neo_link_parser_reset();
        if (ch == prefix[0]) {
            s_state = PARSE_PREFIX;
            s_prefix_idx = 1;
        }
        return false;

    case PARSE_HEX:
        if (ch == '|') {
            s_state = PARSE_SUFFIX_BAR;
            return false;
        }
        if (!isxdigit((unsigned char)ch)) {
            neo_link_parser_reset();
            return false;
        }
        if (s_hex_len >= NEO_LINK_MAX_HEX) {
            neo_link_parser_reset();
            return false;
        }
        s_hex[s_hex_len++] = (char)tolower((unsigned char)ch);
        return false;

    case PARSE_SUFFIX_BAR:
        if (ch == '~') {
            s_hex[s_hex_len] = '\0';
            bool ok = neo_link_parse_hex_payload(s_hex, s_hex_len, out);
            neo_link_parser_reset();
            return ok;
        }
        neo_link_parser_reset();
        return false;

    default:
        neo_link_parser_reset();
        return false;
    }
}

size_t neo_link_format_chat_frame(const char *prompt, char *out, size_t out_size)
{
    if (!prompt || !out || out_size < 16) {
        return 0;
    }

    size_t prompt_len = strlen(prompt);
    if (prompt_len > NEO_LINK_MAX_PAYLOAD) {
        return 0;
    }

    int written = snprintf(out, out_size, "%s01", NEO_LINK_FRAME_PREFIX);
    if (written < 0 || (size_t)written >= out_size) {
        return 0;
    }
    size_t pos = (size_t)written;

    for (size_t i = 0; i < prompt_len; i++) {
        if (pos + 2 >= out_size) {
            return 0;
        }
        written = snprintf(out + pos, out_size - pos, "%02x", (unsigned char)prompt[i]);
        if (written != 2) {
            return 0;
        }
        pos += 2;
    }

    if (pos + sizeof(NEO_LINK_FRAME_SUFFIX) > out_size) {
        return 0;
    }
    memcpy(out + pos, NEO_LINK_FRAME_SUFFIX, sizeof(NEO_LINK_FRAME_SUFFIX));
    return pos + sizeof(NEO_LINK_FRAME_SUFFIX) - 1;
}

bool neo_link_emit_chat_frame(const char *prompt, neo_link_emit_fn emit, void *ctx)
{
    /* Prefer BSS on m68k; ESP can afford a modest stack frame. */
#if defined(__m68k__) || defined(__mc68000__)
    static char frame[NEO_LINK_MAX_HEX + 32];
#else
    char frame[NEO_LINK_MAX_HEX + 32];
#endif
    size_t len = neo_link_format_chat_frame(prompt, frame, sizeof(frame));
    if (len == 0 || !emit) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        emit(frame[i], ctx);
    }
    return true;
}
