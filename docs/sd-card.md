# Optional microSD card

The Buddy can keep backups on a **3.3 V SPI** microSD breakout. This is optional; internal flash works without it.

| Breakout pin | ESP32-S3 GPIO (reference wiring) |
| --- | --- |
| `VCC` | `3V3` only |
| `GND` | `GND` |
| `CS` | GPIO15 |
| `MOSI` / `DI` | GPIO16 |
| `MISO` / `DO` | GPIO17 |
| `SCK` / `CLK` | GPIO18 |

Do not attach a 5 V-only module to these pins. The firmware mounts FAT cards at `/sdcard`. It never formats a card on its own. Factory reset and flash cleanup **do not delete SD files**.

Power down or restart before pulling the card out.

See also [Using Neo2 Buddy](using.md) (backups) and [Cable & power](cable.md).
