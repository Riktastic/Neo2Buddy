# Neo2 Buddy 1.0.0

First stable release of the ESP32-S3 buddy for AlphaSmart Neo2: backup, web portal, Bluetooth keyboard relay, cloud sync, and Setup flasher.

## Highlights

- **Portal** — documents, backups, applets management, typing view, settings, user guide
- **USB Neo manager** — scan / read / write / clear AlphaWord files; applet install from file; autobackup on connect
- **Bluetooth HID** — pair window from the portal; Neo keys pass through to a host
- **Cloud sync** — WebDAV / S3-compatible destinations
- **Flash Cards deck library** — edit named decks on the buddy and push to the Neo
- **Setup** — download `Setup-windows`, `Setup-macos`, or `Setup-linux` from the GitHub release. Unzip and run; no extra software. The app walks you through preparing the ESP32.

## 1.0.0 notes

- **Applet Store installs are paused** while stock SmartApplets get more testing. The store UI shows “Store paused”; Flash Cards deck editing still works. Re-enable with `STOCK_STORE_INSTALLS_ENABLED 1` in `firmware/main/services/stock_applets.c`.
- Primary testing used a **Dutch-market Neo2**. Other regional layouts may differ for live typing, Bluetooth, and import/export.
- Neo USB-B needs **5 V** for reliable enumeration (see `docs/cable.md`).

## Install

| Asset | Use |
|-------|-----|
| `Setup-windows-1.0.0.zip` | Windows — `Neo2BuddySetup.exe` (no extra software) |
| `Setup-macos-1.0.0.zip` | macOS — `Neo2BuddySetup.app` |
| `Setup-linux-1.0.0.zip` | Linux — `./Neo2BuddySetup` |
| Source code (zip / tar.gz) | Attached by GitHub for the tag |

After flash: join the buddy Wi‑Fi (fresh SoftAP: http://192.168.4.1/) and complete first-time setup.

## Publish checklist

1. Commit source + `releases/1.0.0/` (and lean folders / zips you want on the tag).
2. Tag `v1.0.0` and push — `.github/workflows/release.yml` attaches Setup/firmware zips and builds macOS/Linux native Setup when images are present.
3. Or create the release manually with the zips listed above.
