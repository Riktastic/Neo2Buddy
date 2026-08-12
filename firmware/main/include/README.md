Include (Public headers)
========================

This directory holds public headers exposing service APIs, board
configuration constants and small protocol helpers used across the firmware.

Files of interest
-----------------

- `board_config.h` — pin definitions and `HAVE_*` compile-time macros
  (`HAVE_WIFI_WEB`, `HAVE_BLE`, `HAVE_STOCK_APPLETS`, OLED/SD/battery, …).
- `settings.h` — device settings API (NVS-backed).
- `sd_card.h` — SD card mount and helper functions.
- `battery.h` — battery ADC conversion helpers.

Guidelines
----------

- Keep headers stable and avoid exporting FreeRTOS internals from here.
- Add Doxygen comments to every public function to help cross-module
  documentation and maintainability.
