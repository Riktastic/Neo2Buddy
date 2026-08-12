# Neo2 Buddy 1.0.0

First stable release of the ESP32-S3 buddy for AlphaSmart Neo2: backup, web portal, Bluetooth keyboard relay, cloud sync, and Setup flasher.

## Highlights

- **Portal** — documents, backups, applets management, typing view, settings, user guide
- **USB Neo manager** — scan / read / write / clear AlphaWord files; applet install from file; autobackup on connect
- **Bluetooth HID** — pair window from the portal; Neo keys pass through to a host
- **Cloud sync** — WebDAV / S3-compatible destinations
- **Flash Cards deck library** — edit named decks on the buddy and push to the Neo
- **Setup utility** — flash Full / Headless / UART-slim images without ESP-IDF (Python zip + native Windows build)

## 1.0.0 notes

- **Applet Store installs are paused** while stock SmartApplets get more testing. The store UI shows “Store paused”; Flash Cards deck editing still works. Re-enable with `STOCK_STORE_INSTALLS_ENABLED 1` in `firmware/main/services/stock_applets.c`.
- Primary testing used a **Dutch-market Neo2**. Other regional layouts may differ for live typing, Bluetooth, and import/export.
- Neo USB-B needs **5 V** for reliable enumeration (see `docs/neo2-usb-wiring.md`).

## Install

| Asset | Use |
|-------|-----|
| `neo2buddy-setup-windows-1.0.0.zip` | Windows — run `Neo2BuddySetup.exe` (no Python) |
| `neo2buddy-setup-1.0.0.zip` | Any OS — Python 3.10+ (`Run Setup.bat` / `run-setup.sh`) |
| `neo2buddy-firmware-1.0.0.zip` | Raw Full bins for esptool |
| `neo2buddy-firmware-1.0.0-headless.zip` | No OLED / microSD |
| `neo2buddy-firmware-1.0.0-uart-slim.zip` | Serial + Neo USB only |

After flash: join the buddy Wi‑Fi (fresh SoftAP: http://192.168.4.1/) and complete first-time setup.

## Publish checklist

1. Commit source + `releases/1.0.0/` (and lean folders / zips you want on the tag).
2. Tag `v1.0.0` and push — `.github/workflows/release.yml` attaches Setup/firmware zips and builds macOS/Linux native Setup when images are present.
3. Or create the release manually with the zips listed above.
