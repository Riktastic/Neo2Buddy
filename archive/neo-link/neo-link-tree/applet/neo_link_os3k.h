/**
 * @file neo_link_os3k.h
 * @brief OS3K LCD layout helpers for Neo Link applets (1-based coordinates).
 *
 * Reference: isotherm/betawise on GitHub — HelloWorld, DebugTool, os3k/os3k.c
 * Full notes: neo-link/docs/os3k-applet.md
 */

#pragma once

#include <stdint.h>
#include "neo_link_limits.h"

/*
 * OS3K row/col are 1-based (NOT C array indices).
 * SetCursor computes: x = (col - 1) * font_width
 * col 0 underflows → wild pointer → Dutch "Adresfout" near 0x5Cxxxx
 * (cursor struct at 0x5C68 in betawise os3k.c).
 *
 * Neo2 with 16px terminal font: 8 rows (1..8), 40 cols (1..40).
 */

#define NEO_LCD_COL_FIRST 1
#define NEO_LCD_COL_LAST NEO_LINK_LCD_COLS

#define NEO_LCD_ROW_TITLE 1
#define NEO_LCD_ROW_USB 2
#define NEO_LCD_ROW_VIEW_TOP 3
#define NEO_LCD_VIEW_ROWS 4
#define NEO_LCD_ROW_HINT 7
#define NEO_LCD_ROW_STATUS 8
#define NEO_LCD_ROW_INPUT 8

#if NEO_LCD_ROW_VIEW_TOP < 1
#error invalid view top
#endif
#if (NEO_LCD_ROW_VIEW_TOP + NEO_LCD_VIEW_ROWS - 1) > 6
#error viewport must end at or before row 6 (hint on 7, status on 8)
#endif
#if NEO_LCD_ROW_HINT > NEO_LINK_LCD_ROWS || NEO_LCD_ROW_STATUS > NEO_LINK_LCD_ROWS || \
    NEO_LCD_ROW_INPUT > NEO_LINK_LCD_ROWS
#error LCD rows out of range (max 8 for 16px font)
#endif
#if NEO_LCD_COL_LAST > NEO_LINK_LCD_COLS
#error LCD cols out of range (max 40)
#endif
