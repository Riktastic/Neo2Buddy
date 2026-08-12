/**
 * @file neo_link_applet_guard.h
 * @brief Compile-time guards for Neo Link SmartApplets (include from applet .c only).
 *
 * See neo-link/docs/requirements.md and neo-link/docs/os3k-applet.md.
 */

#pragma once

#include "neo_link_limits.h"
#include "neo_link_os3k.h"

/* --- Cross-limit consistency (firmware + applet share neo_link_limits.h) --- */

#if NEO_LINK_APPLET_MAILBOXES_ENABLED
#if NEO_LINK_APPLET_FILE_USAGE < (2 * NEO_LINK_MAILBOX_CAP)
#error NEO_LINK_APPLET_FILE_USAGE must fit two NEO_LINK_MAILBOX_CAP slots
#endif
#endif

#if NEO_LINK_OUTBOX_CAP < (NEO_LINK_PROMPT_MAX + 1)
#error NEO_LINK_OUTBOX_CAP must hold a full prompt
#endif

#if NEO_LINK_REPLY_MAX > NEO_LINK_MAILBOX_CAP
#error NEO_LINK_REPLY_MAX cannot exceed mailbox slot size
#endif

#if NEO_LINK_APPLET_RAM_LEGACY_MAX > NEO_LINK_APPLET_RAM_BUDGET
#error RAM_LEGACY_MAX cannot exceed RAM_BUDGET
#endif

/* --- LCD (1-based). Never pass row/col 0 to OS3K APIs. --- */

#define NEO_LINK_LCD_ROW_VALID(row) ((row) >= 1 && (row) <= NEO_LINK_LCD_ROWS)
#define NEO_LINK_LCD_COL_VALID(col) ((col) >= 1 && (col) <= NEO_LINK_LCD_COLS)

#if !NEO_LINK_LCD_ROW_VALID(NEO_LCD_ROW_TITLE)
#error invalid ROW_TITLE
#endif
#if !NEO_LINK_LCD_ROW_VALID(NEO_LCD_ROW_USB)
#error invalid ROW_USB
#endif
#if !NEO_LINK_LCD_ROW_VALID(NEO_LCD_ROW_HINT)
#error invalid ROW_HINT
#endif
#if !NEO_LINK_LCD_ROW_VALID(NEO_LCD_ROW_STATUS)
#error invalid ROW_STATUS
#endif
#if !NEO_LINK_LCD_COL_VALID(NEO_LCD_COL_FIRST)
#error invalid COL_FIRST
#endif
#if !NEO_LINK_LCD_COL_VALID(NEO_LCD_COL_LAST)
#error invalid COL_LAST
#endif

/* --- Applet coding rules (enforced by review + verify_neo_link_applet.py) ---
 * - Link at most NEO_LINK_APPLET_MAX_LINK_OBJECTS translation units (+ libos3k).
 * - Do NOT link neo_link_protocol.c or neo_link_snprintf.c on Neo.
 * - No char buf[> NEO_LINK_APPLET_STACK_BUF_MAX] on the stack in applet code.
 * - No FileOpen on MSG_SETFOCUS.
 */
