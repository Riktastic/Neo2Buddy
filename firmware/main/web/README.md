Web (HTTP API and SPA)
======================

This folder contains the HTTP endpoints and the Single-Page Application (SPA)
assets used to manage the device over the network. The server-side code
implements request validation, authentication and delegates work to the
`services/` modules.

Key files
---------

- `web_api_http.c` — registers HTTP routes and handlers (status, files,
  keyboard, settings, command endpoints).
- `firmware-web/` — SPA static files (HTML/CSS/JS) packed into LittleFS and served by the firmware.

Guidelines
---------

- Keep HTTP handlers minimal: validate inputs, check auth, call into services.
- Use `cJSON` (or similar) for JSON parsing rather than naive string parsing.
- Do not perform hardware I/O directly in HTTP handlers; use `services/`.

TODOs
-----

- Replace existing string-based JSON parsing with `cJSON` and strict
  validation in `web_api_http.c`.
- Add authentication rate-limiting and token expiry.
