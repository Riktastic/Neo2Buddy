/**
 * @file board_config.h
 * @brief Per-board GPIO and hardware configuration constants.
 *
 * Centralized hardware mappings for the development kit and target board
 * so other modules can use named constants rather than magic numbers.
 */

#pragma once

#include "driver/gpio.h"

#ifdef __has_include
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/** Display defaults (for OLED or other display variants). */
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 240

#define BOARD_LCD_BACKLIGHT_GPIO GPIO_NUM_2
#define BOARD_LCD_DC_GPIO GPIO_NUM_8
#define BOARD_LCD_CS_GPIO GPIO_NUM_9
#define BOARD_LCD_CLK_GPIO GPIO_NUM_10
#define BOARD_LCD_MOSI_GPIO GPIO_NUM_11
#define BOARD_LCD_MISO_GPIO GPIO_NUM_12
#define BOARD_LCD_RESET_GPIO GPIO_NUM_14

/* I2C pins for 1.3" OLED on Olimex pUEXT (matches Arduino variant SDA=48, SCL=47).
 * Do NOT use GPIO6 for I2C — it is BAT_SENSE on the DevKit-Lipo PCB. */
#define BOARD_I2C_SDA_GPIO GPIO_NUM_48
#define BOARD_I2C_SCL_GPIO GPIO_NUM_47
#define BOARD_TOUCH_RESET_GPIO GPIO_NUM_13
#define BOARD_TOUCH_INTERRUPT_GPIO GPIO_NUM_5

/* On-board LiPo divider sense (Olimex BAT_SENSE). */
#define BOARD_BATTERY_ADC_GPIO GPIO_NUM_6
/* Ratio of the divider network used to scale battery voltage into ADC range. */
#define BOARD_BATTERY_DIVIDER_RATIO 3.0f

/* Optional external 3.3 V microSD breakout on the board's SH1.0 GPIO header. */
/* It uses a dedicated SPI bus so LCD traffic cannot interrupt file transfers. */
#define BOARD_SD_CS_GPIO GPIO_NUM_15
#define BOARD_SD_MOSI_GPIO GPIO_NUM_16
#define BOARD_SD_MISO_GPIO GPIO_NUM_17
#define BOARD_SD_CLK_GPIO GPIO_NUM_18

/*
 * USB host (AlphaSmart Neo2) — Olimex ESP32-S3-DevKit-LiPo:
 *   OTG1  = native ESP32 USB (GPIO19/20) — Neo2 data connection.
 *   Other USB-C = CH340 serial (COM port for flash/monitor).
 * Neo2 power: **5 V required on USB-B** for emulation/USB (power bank OK). Internal
 * AAs run normal writing only — they do not wake USB for the buddy. See docs/neo2-usb-wiring.md.
 */
#define BOARD_USB_OTG_PORT_LABEL "OTG1"

/* Feature availability macros set from Kconfig options. */
#ifdef CONFIG_SUPPORT_BATTERY
#define HAVE_BATTERY 1
#else
#define HAVE_BATTERY 0
#endif

#ifdef CONFIG_SUPPORT_SDCARD
#define HAVE_SDCARD 1
#else
#define HAVE_SDCARD 0
#endif

#ifdef CONFIG_SUPPORT_OLED
#define HAVE_OLED 1
#else
#define HAVE_OLED 0
#endif

#ifdef CONFIG_SUPPORT_WIFI_WEB
#define HAVE_WIFI_WEB 1
#else
#define HAVE_WIFI_WEB 0
#endif

#ifdef CONFIG_SUPPORT_BLE
#define HAVE_BLE 1
#else
#define HAVE_BLE 0
#endif

#ifdef CONFIG_SUPPORT_STOCK_APPLETS
#define HAVE_STOCK_APPLETS 1
#else
#define HAVE_STOCK_APPLETS 0
#endif
