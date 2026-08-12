/**
 * @file neo_link_emit.c
 * @brief Uplink: NeoLinkOut mailbox (primary) + optional HID HLT stream.
 *
 * Neo OS takes the LCD for "Attached to PC, emulating keyboard" while USB is
 * plugged — applets cannot keep focus. Path 2 (NeoLinkOut) works unplugged;
 * HID is best-effort only when still focused with USB up.
 */

#include "os3k.h"
#include "neo_link_limits.h"
#include "neo_link_inbox.h"

#include <string.h>

static char s_out_slot[NEO_LINK_OUTBOX_CAP];

static KeyMod_e neo_link_char_to_key(char c)
{
    switch (c) {
    case '~':
        return (KeyMod_e)(KEY_GRAVE | KEY_MOD_LEFTSHIFT);
    case '|':
        return (KeyMod_e)(KEY_BACKSLASH | KEY_MOD_LEFTSHIFT);
    case 'H':
        return (KeyMod_e)(KEY_H | KEY_MOD_LEFTSHIFT);
    case 'L':
        return (KeyMod_e)(KEY_L | KEY_MOD_LEFTSHIFT);
    case 'T':
        return (KeyMod_e)(KEY_T | KEY_MOD_LEFTSHIFT);
    case '0':
        return (KeyMod_e)KEY_0;
    case '1':
        return (KeyMod_e)KEY_1;
    case '2':
        return (KeyMod_e)KEY_2;
    case '3':
        return (KeyMod_e)KEY_3;
    case '4':
        return (KeyMod_e)KEY_4;
    case '5':
        return (KeyMod_e)KEY_5;
    case '6':
        return (KeyMod_e)KEY_6;
    case '7':
        return (KeyMod_e)KEY_7;
    case '8':
        return (KeyMod_e)KEY_8;
    case '9':
        return (KeyMod_e)KEY_9;
    case 'a':
        return (KeyMod_e)KEY_A;
    case 'b':
        return (KeyMod_e)KEY_B;
    case 'c':
        return (KeyMod_e)KEY_C;
    case 'd':
        return (KeyMod_e)KEY_D;
    case 'e':
        return (KeyMod_e)KEY_E;
    case 'f':
        return (KeyMod_e)KEY_F;
    default:
        return (KeyMod_e)KEY_NONE;
    }
}

static void neo_link_queue_emit(char c)
{
    KeyMod_e key = neo_link_char_to_key(c);
    if ((key & 0xFF) == KEY_NONE) {
        return;
    }
    if (key & KEY_MOD_SHIFT) {
        SetKeyModifiers((uint16_t)(KEY_MOD_LEFTSHIFT >> 8));
    } else {
        SetKeyModifiers(0);
    }
    QueueKey(key);
    SleepCentiseconds(3);
    SetKeyModifiers(0);
    SleepCentiseconds(1);
}

static void neo_link_emit_hex_byte(unsigned char b)
{
    static const char *hex = "0123456789abcdef";
    neo_link_queue_emit(hex[(b >> 4) & 0xF]);
    neo_link_queue_emit(hex[b & 0xF]);
}

static bool neo_link_emit_hid_stream(const char *prompt)
{
    size_t i;
    size_t len;

    if (!prompt) {
        return false;
    }
    len = strlen(prompt);
    if (len == 0 || len > NEO_LINK_PROMPT_MAX) {
        return false;
    }

    neo_link_queue_emit('~');
    neo_link_queue_emit('|');
    neo_link_queue_emit('H');
    neo_link_queue_emit('L');
    neo_link_queue_emit('T');
    neo_link_queue_emit('1');
    neo_link_queue_emit('|');
    neo_link_emit_hex_byte(0x01); /* CHAT */
    for (i = 0; i < len; i++) {
        neo_link_emit_hex_byte((unsigned char)prompt[i]);
    }
    neo_link_queue_emit('|');
    neo_link_queue_emit('~');
    return true;
}

static bool neo_link_write_outbox(const char *prompt)
{
    size_t len;
    int open_rc;

    if (!prompt || prompt[0] == '\0') {
        return false;
    }
    len = strlen(prompt);
    if (len >= NEO_LINK_OUTBOX_CAP) {
        len = NEO_LINK_OUTBOX_CAP - 1;
    }
    memset(s_out_slot, 0, sizeof(s_out_slot));
    memcpy(s_out_slot, prompt, len);

    open_rc = FileOpen(NEO_LINK_MAILBOX_FILE_OUT);
    if (!open_rc) {
        open_rc = FileOpen(NEO_LINK_MAILBOX_FILE_OUT);
    }
    if (!open_rc) {
        return false;
    }
    (void)FileWriteBuffer(s_out_slot, (uint16_t)sizeof(s_out_slot));
    FileClose();
    return true;
}

bool neo_link_applet_send_chat(const char *prompt, bool try_hid)
{
    bool out_ok;
    bool hid_ok = false;

    if (!prompt || prompt[0] == '\0') {
        return false;
    }
    /* Path 2: local NeoLinkOut — works while unplugged (OS emulate screen N/A). */
    out_ok = neo_link_write_outbox(prompt);
    if (try_hid) {
        hid_ok = neo_link_emit_hid_stream(prompt);
    }
    return out_ok || hid_ok;
}
