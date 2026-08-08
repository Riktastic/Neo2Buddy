/**
 * @file uart_cmd.h
 * @brief Password-protected UART REPL (115200, CH340 serial).
 *
 * Linux-style help, tab completion, and `neo` / `buddy` command groups mirror
 * the web API for field testing without Wi-Fi. Login uses the same portal
 * password as auth.c. Requires CONFIG_BUDDY_UART_CMD at build time.
 */

#pragma once

#include "esp_err.h"

/** Start the interactive UART REPL (requires CONFIG_BUDDY_UART_CMD). */
esp_err_t uart_cmd_init(void);
