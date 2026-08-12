/**
 * @file neo_link_limits.h
 * @brief Cross-boundary limits for Neo Link (applet ↔ ESP32 firmware).
 *
 * Single source of truth — change here, rebuild applet + firmware together.
 * See neo-link/docs/requirements.md and neo-link/docs/os3k-applet.md.
 */

#pragma once

/** SmartApplet id — Neo Link Chat (distinct from HelloWorld 0xA1A0). */
#define NEO_LINK_APPLET_ID 0xA1C0

/**
 * NeoLinkIn (file 1) + NeoLinkOut (file 2) mailbox slots.
 * Header: fileCount=2, fileUsage=NEO_LINK_APPLET_FILE_USAGE.
 */
#define NEO_LINK_APPLET_MAILBOXES_ENABLED 1

/** Applet header fileCount and ASM file indices (1-based on device). */
#define NEO_LINK_MAILBOX_FILE_IN 1
#define NEO_LINK_MAILBOX_FILE_OUT 2

/**
 * Total file_space reserved in applet header (ramUsage includes this).
 * Must be >= 2 * NEO_LINK_MAILBOX_CAP.
 */
#define NEO_LINK_APPLET_FILE_USAGE 1024

/** Fixed ASM slot size for NeoLinkIn / NeoLinkOut (buddy writes full slot). */
#define NEO_LINK_MAILBOX_CAP 512

/** Max reply text shown on Neo LCD (FileReadBuffer length + wrap buffer). */
#define NEO_LINK_REPLY_MAX 200

/** Max compose length in TextBox and NeoLinkOut payload. */
#define NEO_LINK_PROMPT_MAX 60

/** NeoLinkOut FileWriteBuffer slot size in applet BSS (prompt max 60). */
#define NEO_LINK_OUTBOX_CAP 128

/**
 * HLT CHAT payload max for ESP32 decoder (neo_link_protocol).
 * Neo applet streams frames char-by-char; do not link protocol.c on Neo.
 */
#define NEO_LINK_HLT_PAYLOAD_MAX 512

/** Target: applet ramUsage (BSS) should stay under ~1.5 KB. See os3k-applet.md. */
#define NEO_LINK_APPLET_RAM_BUDGET 1536

/** Buddy auto-replaces installed applets above this ramUsage (legacy crash builds). */
#define NEO_LINK_APPLET_RAM_LEGACY_MAX 800

/** Neo2 @ 16px system font: rows and cols are 1-based in OS3K APIs. */
#define NEO_LINK_LCD_ROWS 8
#define NEO_LINK_LCD_COLS 40

/** Max stack buffer in any applet function (BSS/static for anything larger). */
#define NEO_LINK_APPLET_STACK_BUF_MAX 64

/** Applet link set: NeoLinkChat.o + neo_link_emit.o only (+ libos3k). */
#define NEO_LINK_APPLET_MAX_LINK_OBJECTS 2

/** Bundled NeoLinkChat version (must match NeoLinkChat.c APPLET_VERSION). */
#define NEO_LINK_APPLET_VERSION_MAJOR 0
#define NEO_LINK_APPLET_VERSION_MINOR 9
#define NEO_LINK_APPLET_VERSION_MINOR_STR "9"
#define NEO_LINK_APPLET_VERSION_REV 'a'
#define NEO_LINK_APPLET_VERSION_REV_STR "a"
