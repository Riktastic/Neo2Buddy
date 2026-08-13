# For developers

End users should use the [Setup app from a release](flashing.md). This page is the map of the repository and how to build or package firmware.

## Repository

| Path | Role |
|------|------|
| `firmware/` | ESP-IDF firmware (Neo USB, services, HTTP API) |
| `firmware-web/` | Browser UI, packed into LittleFS at build time |
| `firmware/main/web/` | C HTTP API (not the HTML) |
| `flasher/` | Setup GUI and Image Builder |
| `python-wrapper/` | Network CLI / library (`neo2buddy`) |
| `releases/` | Flashable packs (`1.0.0`, `-headless`, `-uart-slim`, `custom/`) |
| `docs/` | User and hardware documentation |
| `samples/` / `tools/` | Stock applet sources and helper scripts |
| `specs/` | Product behaviour notes |

## Flash and build (three official paths)

Canonical write-up: **[Flashing](flashing.md)**. End users download **Setup** (`Setup-windows` / `Setup-macos` / `Setup-linux`). GitHub also attaches **Source code**. ESP-IDF and Image Builder are for custom builds.

Isolated custom builds (do not overwrite `firmware/sdkconfig`):

```powershell
cd firmware
. .\idf_env.ps1
idf.py -B build-custom/uart-slim `
  -D SDKCONFIG=build-custom/uart-slim/sdkconfig `
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.d/profile_uart_slim.defaults" `
  build
```

Kconfig flags: **Neo2 Buddy Configuration** in `firmware/main/Kconfig`. Fragments: `firmware/sdkconfig.d/profile_*.defaults`.

## Package a release

```powershell
.\firmware\scripts\package-release.ps1 -Version 1.0.0
.\firmware\scripts\package-profiles.ps1 -Version 1.0.0
.\firmware\scripts\package-profile.ps1 -Version 1.0.0 -Profile uart-slim -BuildDir firmware\build-custom\uart-slim
```

Windows Setup zip from already-packaged images:

```powershell
.\flasher\scripts\build-windows.ps1 -Version 1.0.0
```

Writes `releases/1.0.0/Setup-windows-1.0.0.zip`. Linux and macOS Setup zips are produced by the release workflow.

Unit tests:

```powershell
idf.py -C firmware/test set-target esp32s3
idf.py -C firmware/test build
```

## Other technical notes

- [Firmware README](../firmware/README.md) — IDF env, profiles
- [Neo USB design](../firmware/docs/neo-usb-and-backup.md)
- [Cloud sync API](../firmware/docs/cloud-sync.md)
- [Python wrapper](../python-wrapper/README.md)
- [Setup / Image Builder source](../flasher/README.md)
- [Portal demo internals](demo.md)

Stock Applet Store installs are gated with `STOCK_STORE_INSTALLS_ENABLED` in `firmware/main/services/stock_applets.c`.
