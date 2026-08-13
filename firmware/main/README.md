# Firmware Module Guide

`main.c` is deliberately small: it initializes storage and services, then starts
the local management server.

## Folders

- `include/` contains board configuration and stable, cross-module service
  headers.
- `neo/` contains the AlphaSmart NEO protocol implementation: framing,
  transport dialogue, text conversion, document import, and applet operations.
- `services/` contains device-local concerns that do not belong to the NEO
  protocol, including authentication, battery conversion, storage mounting,
  status, settings, and the live keyboard buffer.
- `web/` contains HTTP routing and request/response handling only. It delegates
  device work to `neo/` and `services/` modules. Static portal assets live in
  repo-root `firmware-web/` (packed into LittleFS at build time).

## READMEs

- `neo/README.md` — protocol design and NEO transport responsibilities.
- `services/README.md` — responsibilities for services (neo_live, settings,
  battery, sd_card) and where to add board-specific helpers.
- `web/README.md` — describes the HTTP API, authentication, and SPA files.

## Module Contracts

- `neo_message` creates and validates the eight-byte NEO protocol frames.
- `neo_device` implements dialogue and block transfers over `usb_host_neo`.
- `usb_host_neo` is the only layer allowed to own USB-host state.
- `neo_applet` manages SmartApplets after a NEO protocol dialogue begins.
- `neo_import` persists converted backups to removable or internal storage.
- `neo_live` keeps a bounded, thread-safe view of keyboard input for the web UI.
- `web_api_http` authenticates HTTP requests and maps them to service calls.

Keep hardware-specific changes inside `services/` or `usb_host_neo`; keep HTTP
parsing out of protocol modules. Each public function should document ownership,
input validation, and any NEO state transition it performs.

## Build-time Feature Flags

This firmware exposes build-time options for lean boards and custom images.
From the project's firmware directory run `idf.py menuconfig` and look under
**Neo2 Buddy Configuration**, or use Image Builder / `sdkconfig.d` profile
fragments (see [`../README.md`](../README.md)).

| Kconfig | `HAVE_*` macro | When off |
|---------|----------------|----------|
| `Support battery` | `HAVE_BATTERY` | No ADC battery helpers |
| `Support microSD card` | `HAVE_SDCARD` | SPIFFS only |
| `Support 1.3in OLED` | `HAVE_OLED` | No display init |
| `Enable Wi-Fi and web portal` | `HAVE_WIFI_WEB` | UART + Neo USB only (no SoftAP/HTTP/cloud sync) |
| `Enable Bluetooth HID keyboard` | `HAVE_BLE` | No NimBLE HID stack |
| `Enable Applet Store` | `HAVE_STOCK_APPLETS` | No embedded stock applets / portal store (requires Wi-Fi/web) |
| `Enable protected UART command console` | — | No serial REPL |

Macros are defined in `include/board_config.h` so sources can use
`#if HAVE_WIFI_WEB` style guards. Optional components are still listed in
`CMakeLists.txt` `REQUIRES` (ESP-IDF early expansion ignores `CONFIG_*` there);
only the corresponding `.c` files are omitted when a feature is disabled.

## Pinout and Parts (reference for this dev kit)

The project reference hardware used for development and testing is an
Olimex ESP32-S3-DevKit-Lipo with an external microSD adapter and a small
1.3" I2C OLED. The GPIO names below match the constants defined in
`include/board_config.h`.

- OLED display (I2C, 1.3" 128x64):
  - VCC -> 3.3V (do NOT use 5V)
  - GND -> GND
  - SDA -> `BOARD_I2C_SDA_GPIO` (GPIO48, Olimex pUEXT)
  - SCL -> `BOARD_I2C_SCL_GPIO` (GPIO47, Olimex pUEXT)

- microSD card adapter (SPI with level shifter):
  - VCC -> 3.3V (power the adapter at 3.3V)
  - GND -> GND
  - MOSI -> `BOARD_SD_MOSI_GPIO` (default: GPIO16)
  - MISO -> `BOARD_SD_MISO_GPIO` (default: GPIO17)
  - SCK  -> `BOARD_SD_CLK_GPIO`  (default: GPIO18)
  - CS   -> `BOARD_SD_CS_GPIO`   (default: GPIO15)

- Li‑Po battery (JST-PH):
  - Connect to the Olimex JST battery connector (charger onboard).
  - Battery positive to the board JST connector; negative to GND.
  - Battery sense uses `BOARD_BATTERY_ADC_GPIO` (GPIO6, on-board divider)

### Parts list (development reference)

| Part | SKU | Qty | Unit price |
|---|---:|---:|---:|
| MicroSD Kaart Adapter Module 3.3V-5V met Level Shifter | 000375 | 1 | €3.00 |
| Olimex ESP32-S3-DevKit-Lipo | 006173 | 1 | €15.50 |
| PKCELL Li-Po Batterij 3.7V 2000mAh - JST-PH - LP803860 | 005880 | 1 | €7.50 |
| 1.3 inch OLED Display 128x64 pixels White - I2C | 001709 | 1 | €7.00 |

Safety notes:
- Always power the microSD adapter and OLED from 3.3V. Do not connect 5V to
  device GPIOs unless the module explicitly supports 5V signaling.
- Verify CS/MOSI/MISO/SCK wiring before inserting an SD card to avoid
  accidental damage. Use the adapter's level shifter as intended.

## AlphaSmart Neo 2 USB connection

The Neo 2 connects to the ESP32-S3 **OTG1** port (native USB on GPIO19/20). **USB-B
must have 5 V** for emulation/USB — internal AAs run normal writing only. See
[docs/cable.md](../../docs/cable.md) for the split power/data
cable: 5 V on USB-B from a power bank, data via OTG1, live switch to emulation mode
when the buddy is connected (no Neo reboot required).
