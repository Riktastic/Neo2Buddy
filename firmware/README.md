Firmware development notes

- Requires ESP-IDF 5.x or later installed and available in your PATH.
- Set up the ESP-IDF environment before building, e.g. (PowerShell):

```
# from this folder
. .\idf_env.ps1
# or from the ESP-IDF install directory: .\export.ps1
```

- The project expects `IDF_PATH`/env to be configured; run `idf.py build` from `firmware` after sourcing the environment.
- Flashing: `idf.py -p <PORT> flash monitor` (or `.\flash.ps1`)

## Feature profiles (lean / custom images)

Kconfig options live under **Neo2 Buddy Configuration** (`main/Kconfig`). Stock fragments:

| Fragment | Effect |
|----------|--------|
| *(defaults)* | Full: Wi-Fi portal, BLE, Applet Store, OLED, SD |
| `sdkconfig.d/profile_headless.defaults` | No OLED / SD |
| `sdkconfig.d/profile_uart_slim.defaults` | UART + Neo USB only |
| `sdkconfig.d/profile_no_ble.defaults` | Portal features without Bluetooth |

Build with an isolated sdkconfig (do not overwrite the main `firmware/sdkconfig`):

```powershell
. .\idf_env.ps1
idf.py -B build-custom/uart-slim `
  -D SDKCONFIG=build-custom/uart-slim/sdkconfig `
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.d/profile_uart_slim.defaults" `
  build
```

Or use the Tk **Image Builder**: `python -m neo2buddy_flasher.builder_app` from `flasher/` (see [flasher/README.md](../flasher/README.md)).

Package release folders:

```powershell
.\scripts\package-release.ps1 -Version 1.0.0
.\scripts\package-profiles.ps1 -Version 1.0.0
.\scripts\package-profile.ps1 -Version 1.0.0 -Profile uart-slim -BuildDir build-custom\uart-slim
```

## Neo USB & backups (read these)

| Doc | Contents |
|-----|----------|
| [`docs/neo-usb-and-backup.md`](docs/neo-usb-and-backup.md) | What we tried, why flip/cooldown/changed-backup won |
| [`main/neo/README.md`](main/neo/README.md) | Module map and lifecycle |
| [`docs/cloud-sync.md`](docs/cloud-sync.md) | WebDAV / S3 backup upload API |
| [`main/README.md`](main/README.md) | Module guide + build-time feature flags |

- This repository also uses NimBLE and LittleFS; ensure those components are available in your IDF installation.
