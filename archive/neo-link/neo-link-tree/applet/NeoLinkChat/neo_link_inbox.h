/**
 * @file neo_link_inbox.h
 * @brief Undocumented OS3K file syscalls (betawise os3k/syscall.c indices 102–115).
 *
 * Safety (see neo-link/docs/os3k-applet.md):
 * - ensure_mailbox_files() on MSG_INIT seeds empty slots (fileCount=2).
 * - FileOpen on user Find / send path only — never on MSG_SETFOCUS.
 * - FileReadBuffer(buf, NEO_LINK_REPLY_MAX) — never larger than buf.
 * - FileWriteBuffer fixed slot (NEO_LINK_OUTBOX_CAP), not stack buffer.
 */

#pragma once

#include <stdint.h>

int FileOpen(uint8_t file_number);
void FileClose(void);
int FileReadBuffer(void *buffer, uint16_t length);
int FileWriteBuffer(const void *buffer, uint16_t length);

/* Header declares SetKeyModifiers; syscall.o exports SetModifierKeys. */
void SetModifierKeys(uint16_t mask);
#define SetKeyModifiers(mask) SetModifierKeys(mask)
