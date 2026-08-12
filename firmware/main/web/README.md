# Web (HTTP API)

This folder is the **C HTTP layer** only. The browser UI lives in repo-root
`firmware-web/` and is packed into LittleFS at build time.

Compiled only when **`HAVE_WIFI_WEB`** is on (Full / Headless / No-BLE profiles).
UART-slim builds omit this code path entirely.

## Key files

- `web_api_http.c` / `.h` — registers HTTP routes and handlers (status, files,
  keyboard, settings, applets / Applet Store, command endpoints).
- Static SPA — edit `firmware-web/`; do not put HTML/CSS/JS here.

## Guidelines

- Keep HTTP handlers minimal: validate inputs, check auth, call into services.
- Use `cJSON` for JSON parsing rather than naive string parsing.
- Do not perform hardware I/O directly in HTTP handlers; use `services/` or `neo/`.
- Guard SD / stock-applet paths with `#if HAVE_SDCARD` / `#if HAVE_STOCK_APPLETS`
  (include `board_config.h` first).
