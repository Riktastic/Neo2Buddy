Firmware development notes

- Requires ESP-IDF 5.x or later installed and available in your PATH.
- Set up the ESP-IDF environment before building, e.g. (PowerShell):

```
# from ESP-IDF install directory
\> .\export.ps1
# or follow https://docs.espressif.com
```

- The project expects `IDF_PATH`/env to be configured; run `idf.py build` from `firmware` after sourcing the environment.
- Flashing: `idf.py -p <PORT> flash monitor` (or `.\flash.ps1`)

## Neo USB & backups (read these)

| Doc | Contents |
|-----|----------|
| [`docs/neo-usb-and-backup.md`](docs/neo-usb-and-backup.md) | What we tried, why flip/cooldown/changed-backup won |
| [`main/neo/README.md`](main/neo/README.md) | Module map and lifecycle |
| [`docs/cloud-sync.md`](docs/cloud-sync.md) | WebDAV / S3 backup upload API |

- This repository also uses NimBLE and LittleFS; ensure those components are available in your IDF installation.
