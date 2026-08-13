Hardware requirements and notes

User-facing cable and 5 V: [docs/cable.md](../../docs/cable.md). Optional SD: [docs/sd-card.md](../../docs/sd-card.md). Reference board for shipping firmware is an **ESP32-S3 with a dedicated USB OTG host port** (e.g. Olimex ESP32-S3-DevKit-Lipo). The round LCD notes below are historical and are not the shipping UI (the portal is).

Target platform
---------------

This project targets the ESP32-S3 (CONFIG_IDF_TARGET="esp32s3") with native USB OTG host support. The chosen reference board is the ESP32-S3 Touch LCD 1.28 (GC9A01A over SPI, CST816S touch controller over I2C) but the core requirements are:

- ESP32-S3 or equivalent with USB OTG host capability (required for direct NEO device enumeration and bulk transfers).
- Sufficient RAM to run NimBLE, the web server, and file buffering (S3 is preferred over S2 for USB host support).
- 3.3 V logic for peripheral interfaces.

Power and battery
-----------------

- Primary battery: single-cell LiPo pack. Default nominal capacity for this design: 2000 mAh. Update `include/battery.h` if you replace the pack.
- Battery voltage sense must be wired to the ADC channel defined in `board_config.h`; the firmware assumes a resistor divider and uses `BOARD_BATTERY_DIVIDER_RATIO`.
- Recommended charging/protection: use a proper LiPo charger/controller (MCP73831, TP4056 with protection, or equivalent).

Storage and peripherals
-----------------------

- microSD card: optional. Use a 3.3 V-compatible breakout and wire MOSI/MISO/SCLK/CS to the SPI pins defined in `board_config.h`.
- Display: GC9A01A SPI display supported by reference demo; not required for core NEO/BLE features but recommended for on-device UX.

USB/NEO transport notes
-----------------------

- USB OTG host support is mandatory to implement the NEO USB transport (`neo/usb_host_neo.c`). The current repository contains a simulated host for UI development; replace with board-tested host code before fielding.
- Validate VID/PID enumeration and bulk endpoint availability on your chosen board and USB connector wiring.
- A field-tested **split power/data cable** for the AlphaSmart Neo 2 is documented in [docs/cable.md](../../docs/cable.md).

Development and testing
-----------------------

- Build with ESP-IDF targeting `esp32s3`.
- Test USB host functionality on real hardware before relying on NEO operations.

TODOs
-----

- Add a board-specific `board_config.h` example for the reference S3 touch LCD board.
- Provide recommended wiring diagrams for the microSD breakout and USB connector.
  Neo 2 split power/data harness: [docs/cable.md](../../docs/cable.md).
- Add a small hardware validation suite in `services/` that runs self-tests at first boot.
