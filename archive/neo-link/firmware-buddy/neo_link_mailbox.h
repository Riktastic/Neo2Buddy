/**
 * @file neo_link_mailbox.h
 * @brief ASM mailbox files for Neo Link (NeoTools-style raw bytes, not AlphaWord).
 *
 * NeoLinkIn  — buddy → applet (LLM reply)
 * NeoLinkOut — applet → buddy (prompt fallback when HID/QueueKey fails)
 */

#pragma once

#include "esp_err.h"
#include "neo_link_limits.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NEO_LINK_MAILBOX_IN_NAME "NeoLinkIn"
#define NEO_LINK_MAILBOX_OUT_NAME "NeoLinkOut"

/**
 * Write @p utf8_text to NeoLinkIn as plain ASCII (padded to fixed capacity).
 * @param return_to_keyboard  If true, usb_host_neo_restart() after success.
 */
esp_err_t neo_link_mailbox_deliver(const char *utf8_text, bool return_to_keyboard);

void neo_link_mailbox_deliver_async(const char *utf8_text, bool return_to_keyboard);

esp_err_t neo_link_mailbox_last_error(void);

/**
 * Read NeoLinkOut (applet-written prompt). Plain ASCII into @p out.
 * Flips to ASM; optionally returns to keyboard.
 */
esp_err_t neo_link_mailbox_fetch_out(char *out, size_t out_size, size_t *out_len, bool return_to_keyboard);
