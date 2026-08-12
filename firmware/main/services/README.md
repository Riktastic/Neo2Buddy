# Device services (`firmware/main/services`)

Platform-specific helpers consumed by `main/`, `neo/`, and `web/`. Hardware
(GPIO, ADC, SPI, BLE, Wi-Fi) lives here so protocol code stays portable.

**Related headers** in `firmware/main/include/`: `settings.h`, `file_manager.h`,
`device_status.h`, `cloud_sync.h`, `sd_card.h`, `battery.h`, `neo_import.h`,
`self_test.h`, `board_config.h`.

---

## How to read the code

| Area | Files | Role |
|------|-------|------|
| Auth & settings | `auth.c`, `settings.c` | Portal password, bearer tokens, NVS device config |
| Network | `wifi_manager.c`, `captive_dns.c` | AP/STA, recovery hotspot, captive DNS |
| Storage | `sd_card.c`, `sd_format.c`, `file_manager.c` | microSD mount, format, `/api/v1/files` |
| Neo backups | uses `neo_import` (in `neo/`) | Dated `.txt` paths under `/sdcard/neo` |
| Cloud | `cloud_sync.c` | WebDAV / S3 upload of local backups |
| USB Neo typing | `neo_live.c`, `hid_debug.c` | Live text + raw HID ring for keyboard mode |
| BLE relay | `ble_hid*.c` | Type into a paired BLE host |
| UI hardware | `display.c`, `battery*.c` | OLED home screen, battery percent |
| Diagnostics | `log_buffer.c`, `health_check.c`, `uart_cmd.c` | Portal logs, USB watchdog, serial REPL |
| Factory reset | `factory_reset.c` | Wipe NVS + internal backups (SD untouched) |
| Status hub | `device_status.c` | Single snapshot for `/api/v1/status` |

---

## Conventions

- **`board_config.h`** — pins and `HAVE_*` feature flags; check before calling SD/OLED/BLE/Wi‑Fi APIs.
- **NVS namespaces** — `auth`, `device` (settings), `cloud_sync`; never return secrets over HTTP.
- **Async workers** — SD format and cloud sync run in tasks; HTTP returns `started: true` immediately.
- **File headers** — each `.c`/`.h` starts with `@file` / `@brief` explaining what and why.

### Feature profiles

Lean images compile out optional services via Kconfig (`SUPPORT_WIFI_WEB`, `SUPPORT_BLE`, …).
When Wi‑Fi/web or BLE is off, stub sources (`wifi_manager_stub.c`, `cloud_sync_stub.c`,
`ble_hid_stub.c`) keep the same symbols so Neo USB and UART code still link.
UART-slim builds skip SoftAP, HTTP, captive DNS, and cloud sync entirely — use the
serial REPL instead. See [`../README.md`](../README.md) and [`../../README.md`](../../README.md).

---

## Boot order (typical Full build)

1. `settings_load` → `auth_init` → `device_status_init` → `log_buffer_init`
2. `wifi_manager_init` (AP or STA per settings) — skipped / stubbed when `HAVE_WIFI_WEB` is off
3. `usb_host_neo_init`, `neo_autobackup_init`, optional `sd_card_mount_if_present`
4. `display_init`, `battery_monitor_init`, `cloud_sync_init`, `httpd` + `web_api_register`

See `main.c` for the authoritative sequence on your build.
