/**
 * @file neo_usb_hid.c
 * @brief USB HID boot keyboard → text for live monitor (PID 0xBD04 only).
 *
 * Standard 8-byte boot report: [modifier][reserved][key1..key6].
 * We diff key bitmap vs previous report to emit only newly pressed keys.
 * Special Neo keys (File 1..8, Esc, etc.) map to [F1] style tokens for UI.
 */

#include "neo_usb_hid.h"

#include "ble_hid.h"
#include "hid_debug.h"
#include "neo_debug.h"
#include "neo_live.h"
#include "usb_host_neo.h"

#if CONFIG_BUDDY_NEO_LINK
#include "neo_link_transport.h"
#endif

#include "esp_log.h"

#include <string.h>

static const char *TAG = "neo_hid";

static uint8_t s_prev_keys[6];

/* USB HID usage IDs for US QWERTY (no modifier). Index = usage code. */
static const char s_us_noshift[128] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e', [0x09] = 'f', [0x0a] = 'g',
    [0x0b] = 'h', [0x0c] = 'i', [0x0d] = 'j', [0x0e] = 'k', [0x0f] = 'l', [0x10] = 'm', [0x11] = 'n',
    [0x12] = 'o', [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't', [0x18] = 'u',
    [0x19] = 'v', [0x1a] = 'w', [0x1b] = 'x', [0x1c] = 'y', [0x1d] = 'z', [0x1e] = '1', [0x1f] = '2',
    [0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9',
    [0x27] = '0', [0x28] = '\n', [0x2b] = '\t', [0x2c] = ' ', [0x2d] = '-', [0x2e] = '=', [0x2f] = '[',
    [0x30] = ']', [0x31] = '\\', [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.',
    [0x38] = '/',
};

static const char s_us_shift[128] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E', [0x09] = 'F', [0x0a] = 'G',
    [0x0b] = 'H', [0x0c] = 'I', [0x0d] = 'J', [0x0e] = 'K', [0x0f] = 'L', [0x10] = 'M', [0x11] = 'N',
    [0x12] = 'O', [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T', [0x18] = 'U',
    [0x19] = 'V', [0x1a] = 'W', [0x1b] = 'X', [0x1c] = 'Y', [0x1d] = 'Z', [0x1e] = '!', [0x1f] = '@',
    [0x20] = '#', [0x21] = '$', [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(',
    [0x27] = ')', [0x28] = '\n', [0x2b] = '\t', [0x2c] = ' ', [0x2d] = '_', [0x2e] = '+', [0x2f] = '{',
    [0x30] = '}', [0x31] = '|', [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>',
    [0x38] = '?',
};

/** Neo-specific / non-printable HID usages → live-buffer token (verified on Neo2 HID 0xBD04). */
static const char *neo_usb_hid_special_label(uint8_t code)
{
    switch (code) {
    case 0x29:
        return "[Esc]";
    case 0x3a:
        return "[F1]"; /* File 1 */
    case 0x3b:
        return "[F2]";
    case 0x3c:
        return "[F3]";
    case 0x3d:
        return "[F4]";
    case 0x3e:
        return "[F5]";
    case 0x3f:
        return "[F6]";
    case 0x40:
        return "[F7]";
    case 0x41:
        return "[F8]"; /* File 8 */
    case 0x4a:
        return "[Home]";
    case 0x4c:
        return "[Del]";
    case 0x4d:
        return "[End]";
    default:
        return NULL;
    }
}

static bool key_was_pressed(uint8_t code)
{
    if (code == 0) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        if (s_prev_keys[i] == code) {
            return true;
        }
    }
    return false;
}

/** Alt/AltGr (Neo "Option") altered keys — checked before the US QWERTY map. */
static bool neo_usb_hid_alt_glyph(uint8_t mods, uint8_t code, char *out, size_t out_size, size_t *out_len)
{
    if ((mods & 0x44) == 0 || !out || out_size == 0 || !out_len) {
        return false;
    }
    /* Verified on Neo2: Alt+5 (usage 0x22, mod 0x04) is €, not "5". */
    if (code == 0x22 && out_size >= 3) {
        out[0] = (char)0xe2;
        out[1] = (char)0x82;
        out[2] = (char)0xac;
        *out_len = 3;
        return true;
    }
    return false;
}

void neo_usb_hid_reset(void)
{
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
}

void neo_usb_hid_handle_report(const uint8_t *report, size_t length)
{
    if (!report || length < 8) {
        return;
    }

    /* Serial HID dump is opt-in (same as `keyboard keylog on`); ring buffers stay. */
    if (neo_live_get_key_log()) {
        ESP_LOGI(TAG, "HID report: %02x %02x %02x %02x %02x %02x %02x %02x", report[0], report[1],
                 report[2], report[3], report[4], report[5], report[6], report[7]);
    }
    neo_debug_event("HID %02x %02x %02x %02x %02x %02x %02x %02x", report[0], report[1], report[2], report[3],
                    report[4], report[5], report[6], report[7]);
    hid_debug_append(report, length);

    /* Forward live Neo keys to the paired BLE host (raw boot report). */
    ble_hid_passthrough_report(report, length);

    bool shift = (report[0] & 0x22) != 0; /* Left or right shift */
    const char *map = shift ? s_us_shift : s_us_noshift;
    uint8_t mods = report[0];

    for (size_t i = 2; i < 8; i++) {
        uint8_t code = report[i];
        if (code == 0 || key_was_pressed(code)) {
            continue;
        }
        if (code == 0x2a) {
            /* Backspace — remove last live character if possible */
            char bs = '\b';
            usb_host_neo_publish_keyboard_text(&bs, 1);
            continue;
        }
        if (code == 0x39) {
            continue; /* Caps Lock — hardware toggle, not live text */
        }
        char alt_buf[8];
        size_t alt_len = 0;
        if (neo_usb_hid_alt_glyph(mods, code, alt_buf, sizeof(alt_buf), &alt_len)) {
            usb_host_neo_publish_keyboard_text(alt_buf, alt_len);
            continue;
        }
        if (code < sizeof(s_us_noshift) && map[code] != '\0') {
            char ch = map[code];
#if CONFIG_BUDDY_NEO_LINK
            if (ch == '~' || neo_link_transport_in_frame()) {
                char one[2] = { ch, '\0' };
                neo_link_transport_feed_text(one, 1);
                if (neo_link_transport_in_frame()) {
                    continue;
                }
            }
#endif
            usb_host_neo_publish_keyboard_text(&ch, 1);
            continue;
        }
        const char *special = neo_usb_hid_special_label(code);
        if (special) {
            usb_host_neo_publish_keyboard_text(special, strlen(special));
            continue;
        }
        ESP_LOGD(TAG, "HID key usage 0x%02x (unmapped)", code);
    }

    memcpy(s_prev_keys, report + 2, 6);
}
