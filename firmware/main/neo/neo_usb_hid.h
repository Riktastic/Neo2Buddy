/**
 * @file neo_usb_hid.h
 * @brief Decode HID keyboard reports while Neo is in PID 0xBD04 mode.
 *
 * This is SEPARATE from the ASM comms protocol (0xBD01). In keyboard mode the
 * Neo types like a USB keyboard; we decode boot-protocol reports into
 * characters for the portal live view and forward the raw reports over BLE
 * when a host is connected (passthrough).
 *
 * Does not flip to comms — manager/file ops always call ensure_comms() first.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

void neo_usb_hid_handle_report(const uint8_t *report, size_t length);
void neo_usb_hid_reset(void);
