# Neo2 Buddy firmware 1.0.0

## Easiest: Setup utility (no ESP-IDF)

Download **`neo2buddy-setup-1.0.0.zip`** from the [GitHub release](https://github.com/Riktastic/Neo2Buddy/releases) for this version.

1. Unzip it.
2. Double-click `Run Setup.bat` (Windows) or see README inside the zip for macOS/Linux.
3. Plug in the boardâ€™s **programming USB** port, pick the COM port, click **Install firmware**.

Requires [Python 3.10+](https://www.python.org/downloads/) with Tk (included on Windows/macOS).

## Advanced: esptool

Flash with ESP-IDF / esptool (adjust COM port):

```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 alpha_smart_neo2_buddy.bin 0x1C0000 littlefs.bin
```

Or use `flash_args` from this folder.

Portal user guide: `/user-guide.html` after flashing.
Python client: `python-wrapper/` (`neo2buddy` CLI).

## Known behaviour

- Neo USB-B needs 5 V for enumeration (see docs/neo2-usb-wiring.md).
- Backup / scan / read / write interrupt Neo keyboard mode. Backup now/all return to keyboard; a single Read may need Keyboard mode.
- BLE: use **Pair keyboard** in the portal (2-minute window). Neo keys pass through while connected; Documents/ASM actions can pause keystrokes until keyboard mode returns.
- Applet Store installs are **paused** in 1.0.0 while stock SmartApplets receive more testing (binaries remain in firmware; Flash Cards deck library still works). Set `STOCK_STORE_INSTALLS_ENABLED` to 1 to re-enable.
- Optional lean profiles (build with `firmware/scripts/package-profiles.ps1`): `1.0.0-headless`, `1.0.0-uart-slim`. Pick them in Setup â†’ Firmware profile.
- Primary testing: Dutch-market Neo2. Other regional layouts may differ for live typing / Bluetooth / import-export.
