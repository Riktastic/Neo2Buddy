# Firmware

ESP-IDF project for the Neo2 Buddy. **Users:** flash with the [Setup app, ESP-IDF, or Image Builder](../docs/flashing.md). This folder is for building.

## Environment

ESP-IDF **5.3.1**. From this directory:

```powershell
. .\idf_env.ps1
idf.py build
idf.py -p COMx flash monitor
```

Or `.\flash.ps1 COM3`. Feature flags: `main/Kconfig`. Lean fragments: `sdkconfig.d/profile_*.defaults` (use an isolated `SDKCONFIG` under `build-custom/` so you do not overwrite the Full tree). Image Builder: `python -m neo2buddy_flasher.builder_app`.

Packaging and repo map: [docs/developers.md](../docs/developers.md).

## Design notes (not user manuals)

| Doc | Contents |
|-----|----------|
| [docs/neo-usb-and-backup.md](docs/neo-usb-and-backup.md) | USB flip / backup policy |
| [docs/cloud-sync.md](docs/cloud-sync.md) | WebDAV / S3 / Hammer API |
| [main/README.md](main/README.md) | Module guide |
| [main/neo/README.md](main/neo/README.md) | Neo protocol map |

Cable and 5 V: [docs/cable.md](../docs/cable.md).
