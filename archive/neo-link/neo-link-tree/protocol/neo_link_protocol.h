/**
 * @file neo_link_protocol.h
 * @brief Host Link Transport (HLT) — framing codec for ESP32 buddy.
 *
 * ESP32: link neo_link_protocol.c in firmware (keyboard decode path).
 * Neo applet: do NOT link protocol.c — use streamed emit in neo_link_emit.c
 * (see neo-link/docs/os3k-applet.md).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "neo_link_limits.h"

#define NEO_LINK_FRAME_PREFIX "~|HLT1|"
#define NEO_LINK_FRAME_SUFFIX "|~"
#define NEO_LINK_MAX_PAYLOAD NEO_LINK_HLT_PAYLOAD_MAX
#define NEO_LINK_MAX_HEX (NEO_LINK_MAX_PAYLOAD * 2)

typedef enum {
    NEO_LINK_MSG_CHAT = 0x01,
    NEO_LINK_MSG_PING = 0x02,
    NEO_LINK_MSG_ABORT = 0x7F,
} neo_link_msg_type_t;

typedef struct {
    neo_link_msg_type_t type;
    char text[NEO_LINK_MAX_PAYLOAD + 1];
    size_t text_len;
} neo_link_message_t;

void neo_link_parser_reset(void);

/** Feed one ASCII character from the HID decode path. Returns true when @p out is filled. */
bool neo_link_parser_feed(char ch, neo_link_message_t *out);

size_t neo_link_format_chat_frame(const char *prompt, char *out, size_t out_size);

typedef void (*neo_link_emit_fn)(char ch, void *ctx);

/** ESP32 / host only — uses large stack/static buffers unsuitable for Neo applet. */
bool neo_link_emit_chat_frame(const char *prompt, neo_link_emit_fn emit, void *ctx);

bool neo_link_parse_hex_payload(const char *hex, size_t hex_len, neo_link_message_t *out);
