/**
 * @file stock_os3k_files.h
 * @brief File syscalls for stock applets (BetaWise v0.2 unnamed traps → named).
 *
 * FileSetFolder selects which applet's document slots FileOpen(1..N) use.
 * Word Tree uses this to read AlphaWord (0xA000) without the buddy.
 */
#pragma once
#include <stdint.h>

int FileOpen(uint8_t file_number);
void FileClose(void);
int FileReadBuffer(void *buffer, uint16_t length);
int FileWriteBuffer(const void *buffer, uint16_t length);
/** Switch file namespace to another installed applet (pass AppletFindById result). */
int FileSetFolder(uint8_t applet_index);
