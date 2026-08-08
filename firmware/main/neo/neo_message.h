/**
 * @file neo_message.h
 * @brief 8-byte AlphaSmart Manager protocol packets (NeoTools / ASM parity).
 *
 * HOW THE NEO PROTOCOL WORKS (plain language)
 * ===========================================
 * After USB flip to comms mode (PID 0xBD01), the host talks to the Neo's
 * "AlphaSmart Manager" (ASM) firmware using two kinds of traffic:
 *
 *   1) Legacy ASCII handshakes (not 8-byte framed):
 *        - Single byte 0x01 "hello" → Neo replies 2 bytes: protocol version (BE u16).
 *        - 8-byte "?ff\0reset" clears state between commands.
 *        - 8-byte "?Switch" + applet_id BE → Neo replies 8 ASCII bytes "Switched".
 *      These happen inside neo_device_dialogue_start() before any framed command.
 *
 *   2) Framed 8-byte command/response packets (this file):
 *        byte[0] = command or response code (see enum below)
 *        byte[1..6] = arguments (big-endian fields at documented offsets)
 *        byte[7] = checksum = (sum of bytes 0..6) mod 256
 *
 * A typical file read on AlphaWord (applet 0xA000) looks like:
 *   dialogue_start(0x0000 system) → GET_FILE_ATTRIBUTES → read 40-byte attr block
 *   → READ_RAW_FILE → read_extended() pulls payload in 1 KiB BLOCK_READ chunks
 *   → dialogue_end (reset).
 *
 * We emulate NeoTools by sending the same bytes in the same order. Wrong ordering
 * (e.g. reading before writing, or reading 8 bytes for hello) hangs the Neo.
 *
 * Request codes are 0x00..0x1f; matching responses are 0x40..0x5c; errors 0x86+.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/** Eight-byte AlphaSmart Manager protocol command and response constants. */
enum {
    NEO_REQUEST_VERSION = 0x00,
    NEO_REQUEST_UNKNOWN_01 = 0x01,
    NEO_REQUEST_BLOCK_WRITE = 0x02,
    NEO_REQUEST_UNKNOWN_03 = 0x03,
    NEO_REQUEST_LIST_APPLETS = 0x04,
    NEO_REQUEST_REMOVE_APPLET = 0x05,
    NEO_REQUEST_WRITE_APPLET = 0x06,
    NEO_REQUEST_FINALIZE_WRITING_APPLET = 0x07,
    NEO_REQUEST_RESTART = 0x08,
    NEO_REQUEST_SET_BAUDRATE = 0x09,
    NEO_REQUEST_UNKNOWN_0A = 0x0a,
    NEO_REQUEST_PROGRAMMING_APPLET_BLOCK = 0x0b,
    NEO_REQUEST_GET_SETTINGS = 0x0c,
    NEO_REQUEST_SET_SETTINGS = 0x0d,
    NEO_REQUEST_SET_APPLET = 0x0e,
    NEO_REQUEST_READ_APPLET = 0x0f,
    NEO_REQUEST_BLOCK_READ = 0x10,
    NEO_REQUEST_ERASE_APPLETS = 0x11,
    NEO_REQUEST_READ_FILE = 0x12,
    NEO_REQUEST_GET_FILE_ATTRIBUTES = 0x13,
    NEO_REQUEST_WRITE_FILE = 0x14,
    NEO_REQUEST_CONFIRM_WRITE_FILE = 0x15,
    NEO_REQUEST_CLEAR_SEGMENT_MAP = 0x16,
    NEO_REQUEST_ERASE_SEGMENTS = 0x17,
    NEO_REQUEST_SMALL_ROM_UPDATER = 0x18,
    NEO_REQUEST_UNKNOWN_19 = 0x19,
    NEO_REQUEST_GET_AVAIL_SPACE = 0x1a,
    NEO_REQUEST_GET_USED_SPACE = 0x1b,
    NEO_REQUEST_READ_RAW_FILE = 0x1c,
    NEO_REQUEST_SET_FILE_ATTRIBUTES = 0x1d,
    NEO_REQUEST_COMMIT = 0x1e,
    NEO_REQUEST_WRITE_RAW_FILE = 0x1f,

    NEO_RESPONSE_VERSION = 0x40,
    NEO_RESPONSE_UNKNOWN_41 = 0x41,
    NEO_RESPONSE_BLOCK_WRITE = 0x42,
    NEO_RESPONSE_BLOCK_WRITE_DONE = 0x43,
    NEO_RESPONSE_LIST_APPLETS = 0x44,
    NEO_RESPONSE_REMOVE_APPLET = 0x45,
    NEO_RESPONSE_WRITE_APPLET = 0x46,
    NEO_RESPONSE_PROGRAMMING_APPLET_BLOCK = 0x47,
    NEO_RESPONSE_FINALIZE_WRITING_APPLET = 0x48,
    NEO_RESPONSE_UNKNOWN_49 = 0x49,
    NEO_RESPONSE_SET_BAUDRATE = 0x4a,
    NEO_RESPONSE_GET_SETTINGS = 0x4b,
    NEO_RESPONSE_SET_APPLET = 0x4c,
    NEO_RESPONSE_BLOCK_READ = 0x4d,
    NEO_RESPONSE_BLOCK_READ_EMPTY = 0x4e,
    NEO_RESPONSE_ERASE_APPLETS = 0x4f,
    NEO_RESPONSE_WRITE_FILE = 0x50,
    NEO_RESPONSE_CONFIRM_WRITE_FILE = 0x51,
    NEO_RESPONSE_RESTART = 0x52,
    NEO_RESPONSE_READ_FILE = 0x53,
    NEO_RESPONSE_CLEAR_SEGMENT_MAP = 0x54,
    NEO_RESPONSE_ERASE_SEGMENTS = 0x55,
    NEO_RESPONSE_SMALL_ROM_UPDATER = 0x56,
    NEO_RESPONSE_UNKNOWN_57 = 0x57,
    NEO_RESPONSE_GET_AVAIL_SPACE = 0x58,
    NEO_RESPONSE_GET_USED_SPACE = 0x59,
    NEO_RESPONSE_GET_FILE_ATTRIBUTES = 0x5a,
    NEO_RESPONSE_SET_FILE_ATTRIBUTES = 0x5b,
    NEO_RESPONSE_COMMIT = 0x5c,

    NEO_ERROR_INVALID_BAUDRATE = 0x86,
    NEO_ERROR_UNKNOWN_87 = 0x87,
    NEO_ERROR_INVALID_APPLET = 0x8a,
    NEO_ERROR_PROTOCOL = 0x8f,
    NEO_ERROR_PARAMETER = 0x90,
    NEO_ERROR_OUTOFMEMORY = 0x91,
    NEO_ERROR_UNKNOWN_94 = 0x94,
};

/** Fixed-size message container for protocol packets. */
typedef struct {
    uint8_t data[8];
} neo_message_t;

/**
 * Build one 8-byte packet: set cmd in [0], pack big-endian args into [1..6], checksum [7].
 * args is a list of {value, offset, width} tuples; width 0 terminates the list.
 */
void neo_message_init(neo_message_t *m, uint8_t cmd, const uint32_t args[][3]);

/** Sum bytes 0..6, low 8 bits — must match Neo ASM firmware. */
uint8_t neo_message_checksum(const neo_message_t *m);

/** Command/response byte at data[0]. */
uint8_t neo_message_command(const neo_message_t *m);

/** Read big-endian integer from data[offset..offset+width-1]; offset 1..6, width 1..4. */
uint32_t neo_message_argument(const neo_message_t *m, int offset, int width);

/** True when data[7] equals computed checksum. */
bool neo_message_checksum_is_valid(const neo_message_t *m);

/** Human-readable error text for 0x86.. response codes. */
const char *neo_message_error_string(uint8_t command);

/** Short name for logging, e.g. "GET_FILE_ATTRIBUTES". */
const char *neo_message_command_name(uint8_t command);
