# External microSD Card Support

This board has no built-in microSD socket. AlphaTouch Buddy supports a **3.3 V SPI microSD breakout** connected to the spare SH1.0 GPIO header.

| microSD breakout pin | ESP32-S3 board GPIO |
| --- | --- |
| `VCC` | `3V3` only |
| `GND` | `GND` |
| `CS` | GPIO15 |
| `MOSI` / `DI` | GPIO16 |
| `MISO` / `DO` | GPIO17 |
| `SCK` / `CLK` | GPIO18 |

Use a breakout designed for 3.3 V logic. Do not connect a 5 V-only module directly to the ESP32 GPIO pins.

The firmware mounts FAT-formatted cards at `/sdcard`. It never formats a card automatically: a missing, unsupported, or corrupt card reports as unavailable while the device continues to boot from internal storage.

Always eject the card through the future touch or web storage control before physical removal. Until that UI is implemented, restart or power down the device before removing it.

## Related docs

- [AlphaSmart Neo 2 USB wiring (split power/data)](neo2-usb-wiring.md)