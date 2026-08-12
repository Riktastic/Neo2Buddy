# Changelog

## 1.0.0 — 2026-08-12

First stable release (vs `1.0.0-beta.1`).

### Highlights

- Full firmware + Setup flasher (Python zip and native Windows app)
- Lean images: headless, uart-slim
- Portal polish, Neo manager/autobackup, BLE keyboard relay, cloud sync, Flash Cards deck library
- Stock Applet Store present; **installs paused** pending more applet testing

### Added

- Neo2 Buddy Setup (`flasher/`) — GUI flash tool; wipe NVS + LittleFS; profile picker
- Release packaging scripts and GitHub release workflow
- Flash Cards deck library (edit / import / push to Neo)
- Stock SmartApplets embedded + catalog/install API (installs gated off in 1.0.0)
- Firmware feature profiles (full / headless / uart-slim / optional stubs)
- Portal Applet Store dialog and Flash Cards settings entry points
- Python wrapper updates for stock applets and portal APIs

### Changed

- BLE: Neo keystrokes pass through while paired (beta.1 was portal “Send text” only)
- Autobackup on connect returns Neo to keyboard mode after backup
- Backup duplicate detection compares content, not filename
- Setup wipe clears NVS and LittleFS
- Serial boot banner shows portal IP
- Version strings aligned to 1.0.0

### Fixed

- Static asset 404s when URLs include cache-bust `?v=…`
- Sign-in UI trusting stale localStorage tokens without device proof
- Applet Store dialog open/close and install confirm reliability
- Neo documents table column alignment
- Refresh / Scan icon button busy state
- Flash deck “Add card” dropping blank/incomplete rows
- Logs dialog footer rendering in demo mode
- Firmware CI sourcing ESP-IDF export before `idf.py`

### Known limitations

- Applet Store installs paused (`STOCK_STORE_INSTALLS_ENABLED 0`); deck library still works
- Primary testing on Dutch-market Neo2; other layouts may differ
- Neo USB-B needs 5 V for reliable enumeration
