/**
 * @file neo_message.c
 * @brief Build and parse fixed 8-byte Neo Manager packets.
 *
 * Packet layout (all multi-byte fields big-endian):
 *
 *   [0]     command or response ID
 *   [1..6]  arguments (meaning depends on command — see NeoTools source)
 *   [7]     checksum = (byte0 + ... + byte6) & 0xFF
 *
 * Example GET_FILE_ATTRIBUTES for AlphaWord file index 3:
 *   [0]=0x13  [4]=03  [5..6]=applet_id 0xA000  [7]=checksum
 *
 * We never invent new commands here — only pack/unpack what NeoTools already sends.
 */

#include "neo_message.h"
#include <string.h>

void neo_message_init(neo_message_t *m, uint8_t cmd, const uint32_t args[][3])
{
    /* Start from zero so unused argument bytes are 0x00. */
    memset(m->data, 0, sizeof(m->data));
    m->data[0] = cmd;

    if (args) {
        for (size_t i = 0; args[i][2] != 0; ++i) {
            uint32_t value = args[i][0];
            int offset = (int)args[i][1];
            int width = (int)args[i][2];
            /* Arguments live in bytes 1..6 only; byte 7 is reserved for checksum. */
            if (width < 1 || width > 4) {
                continue;
            }
            if (offset < 1 || offset + width > 7) {
                continue;
            }
            /* Pack MSB first — Neo is big-endian on the wire. */
            for (int j = width - 1; j >= 0; --j) {
                m->data[offset + j] = value & 0xFF;
                value >>= 8;
            }
        }
    }

    /* Checksum covers bytes 0..6; store result in byte 7. */
    m->data[7] = neo_message_checksum(m);
}

uint8_t neo_message_checksum(const neo_message_t *m)
{
    uint32_t s = 0;
    for (int i = 0; i < 7; ++i) {
        s += m->data[i];
    }
    return (uint8_t)(s & 0xFF);
}

uint8_t neo_message_command(const neo_message_t *m)
{
    return m->data[0];
}

uint32_t neo_message_argument(const neo_message_t *m, int offset, int width)
{
    if (width < 1 || width > 4) {
        return 0;
    }
    if (offset < 1 || offset + width > 7) {
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < width; ++i) {
        v = (v << 8) | m->data[offset + i];
    }
    return v;
}

bool neo_message_checksum_is_valid(const neo_message_t *m)
{
    return m != NULL && m->data[7] == neo_message_checksum(m);
}

const char *neo_message_error_string(uint8_t command)
{
    switch (command) {
    case NEO_ERROR_INVALID_BAUDRATE:
        return "Bad baud rate";
    case NEO_ERROR_UNKNOWN_87:
    case NEO_ERROR_UNKNOWN_94:
        return "Unknown device error";
    case NEO_ERROR_INVALID_APPLET:
        return "Specified applet ID is not recognised";
    case NEO_ERROR_PROTOCOL:
        return "Protocol error";
    case NEO_ERROR_PARAMETER:
        return "Invalid parameter";
    case NEO_ERROR_OUTOFMEMORY:
        return "Out of memory";
    default:
        return "Unknown response";
    }
}

const char *neo_message_command_name(uint8_t command)
{
    switch (command) {
    case NEO_REQUEST_VERSION:
        return "VERSION";
    case NEO_REQUEST_UNKNOWN_01:
        return "HELLO";
    case NEO_REQUEST_BLOCK_WRITE:
        return "BLOCK_WRITE";
    case NEO_REQUEST_LIST_APPLETS:
        return "LIST_APPLETS";
    case NEO_REQUEST_REMOVE_APPLET:
        return "REMOVE_APPLET";
    case NEO_REQUEST_WRITE_APPLET:
        return "WRITE_APPLET";
    case NEO_REQUEST_FINALIZE_WRITING_APPLET:
        return "FINALIZE_WRITING_APPLET";
    case NEO_REQUEST_RESTART:
        return "RESTART";
    case NEO_REQUEST_SET_BAUDRATE:
        return "SET_BAUDRATE";
    case NEO_REQUEST_PROGRAMMING_APPLET_BLOCK:
        return "PROGRAMMING_APPLET_BLOCK";
    case NEO_REQUEST_GET_SETTINGS:
        return "GET_SETTINGS";
    case NEO_REQUEST_SET_SETTINGS:
        return "SET_SETTINGS";
    case NEO_REQUEST_SET_APPLET:
        return "SET_APPLET";
    case NEO_REQUEST_READ_APPLET:
        return "READ_APPLET";
    case NEO_REQUEST_BLOCK_READ:
        return "BLOCK_READ";
    case NEO_REQUEST_ERASE_APPLETS:
        return "ERASE_APPLETS";
    case NEO_REQUEST_READ_FILE:
        return "READ_FILE";
    case NEO_REQUEST_GET_FILE_ATTRIBUTES:
        return "GET_FILE_ATTRIBUTES";
    case NEO_REQUEST_WRITE_FILE:
        return "WRITE_FILE";
    case NEO_REQUEST_CONFIRM_WRITE_FILE:
        return "CONFIRM_WRITE_FILE";
    case NEO_REQUEST_CLEAR_SEGMENT_MAP:
        return "CLEAR_SEGMENT_MAP";
    case NEO_REQUEST_ERASE_SEGMENTS:
        return "ERASE_SEGMENTS";
    case NEO_REQUEST_SMALL_ROM_UPDATER:
        return "SMALL_ROM_UPDATER";
    case NEO_REQUEST_GET_AVAIL_SPACE:
        return "GET_AVAIL_SPACE";
    case NEO_REQUEST_GET_USED_SPACE:
        return "GET_USED_SPACE";
    case NEO_REQUEST_READ_RAW_FILE:
        return "READ_RAW_FILE";
    case NEO_REQUEST_SET_FILE_ATTRIBUTES:
        return "SET_FILE_ATTRIBUTES";
    case NEO_REQUEST_COMMIT:
        return "COMMIT";
    case NEO_REQUEST_WRITE_RAW_FILE:
        return "WRITE_RAW_FILE";

    case NEO_RESPONSE_VERSION:
        return "VERSION";
    case NEO_RESPONSE_BLOCK_WRITE:
        return "BLOCK_WRITE";
    case NEO_RESPONSE_BLOCK_WRITE_DONE:
        return "BLOCK_WRITE_DONE";
    case NEO_RESPONSE_LIST_APPLETS:
        return "LIST_APPLETS";
    case NEO_RESPONSE_REMOVE_APPLET:
        return "REMOVE_APPLET";
    case NEO_RESPONSE_WRITE_APPLET:
        return "WRITE_APPLET";
    case NEO_RESPONSE_PROGRAMMING_APPLET_BLOCK:
        return "PROGRAMMING_APPLET_BLOCK";
    case NEO_RESPONSE_FINALIZE_WRITING_APPLET:
        return "FINALIZE_WRITING_APPLET";
    case NEO_RESPONSE_SET_BAUDRATE:
        return "SET_BAUDRATE";
    case NEO_RESPONSE_GET_SETTINGS:
        return "GET_SETTINGS";
    case NEO_RESPONSE_SET_APPLET:
        return "SET_APPLET";
    case NEO_RESPONSE_BLOCK_READ:
        return "BLOCK_READ";
    case NEO_RESPONSE_BLOCK_READ_EMPTY:
        return "BLOCK_READ_EMPTY";
    case NEO_RESPONSE_ERASE_APPLETS:
        return "ERASE_APPLETS";
    case NEO_RESPONSE_WRITE_FILE:
        return "WRITE_FILE";
    case NEO_RESPONSE_CONFIRM_WRITE_FILE:
        return "CONFIRM_WRITE_FILE";
    case NEO_RESPONSE_RESTART:
        return "RESTART";
    case NEO_RESPONSE_READ_FILE:
        return "READ_FILE";
    case NEO_RESPONSE_CLEAR_SEGMENT_MAP:
        return "CLEAR_SEGMENT_MAP";
    case NEO_RESPONSE_ERASE_SEGMENTS:
        return "ERASE_SEGMENTS";
    case NEO_RESPONSE_SMALL_ROM_UPDATER:
        return "SMALL_ROM_UPDATER";
    case NEO_RESPONSE_GET_AVAIL_SPACE:
        return "GET_AVAIL_SPACE";
    case NEO_RESPONSE_GET_USED_SPACE:
        return "GET_USED_SPACE";
    case NEO_RESPONSE_GET_FILE_ATTRIBUTES:
        return "GET_FILE_ATTRIBUTES";
    case NEO_RESPONSE_SET_FILE_ATTRIBUTES:
        return "SET_FILE_ATTRIBUTES";
    case NEO_RESPONSE_COMMIT:
        return "COMMIT";

    case NEO_ERROR_INVALID_BAUDRATE:
        return "ERR_INVALID_BAUDRATE";
    case NEO_ERROR_INVALID_APPLET:
        return "ERR_INVALID_APPLET";
    case NEO_ERROR_PROTOCOL:
        return "ERR_PROTOCOL";
    case NEO_ERROR_PARAMETER:
        return "ERR_PARAMETER";
    case NEO_ERROR_OUTOFMEMORY:
        return "ERR_OUTOFMEMORY";
    default:
        return "UNKNOWN";
    }
}
