/**
 * @file display.h
 * @brief On-device OLED status display (128×64 SSD1306 over I2C).
 *
 * Optional hardware (HAVE_OLED). Onboarding screen shows how to join the AP;
 * home screen shows IP, battery, and backup count. While the Neo is in USB
 * keyboard (emulation) mode, the bottom line shows a rolling live-typing
 * preview from neo_live. Brightness follows settings.display_brightness.
 * All drawing runs on a dedicated task.
 */

#pragma once

#include "esp_err.h"

/** Initialize display hardware and start the home-screen refresh task. */
esp_err_t display_init(void);
/** Show Wi-Fi onboarding (SSID, password, URL). */
esp_err_t display_show_onboarding(const char *ssid, const char *password, const char *url);
/** Return to the glanceable home/status screen. */
void display_show_home(void);
/** Request home screen refresh on the display task (safe from Wi-Fi/event handlers). */
void display_request_home(void);
/** Apply contrast from settings (0-100). */
void display_set_brightness(uint8_t percent);
