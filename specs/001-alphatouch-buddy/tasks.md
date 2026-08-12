# Tasks: AlphaSmart Neo2 Buddy

Historical checklist from the AlphaTouch plan. Items marked **done** reflect the shipping Neo2 Buddy. Remaining items are public-beta polish.

## Phase 0 — Spec gate

- [x] T001 Spec / plan reviewed against implemented product (2026-08 update).
- [x] T002 ESP-IDF 5.3.x toolchain in use for builds.
- [x] T003 Partition table: app + OTA meta + LittleFS (see firmware partitions).

## Phase 1 — Board bring-up

- [x] T010 ESP-IDF project + component registration.
- [x] T011 Board constants in `board_config.h` (OLED I2C, SD SPI, battery ADC, OTG1 Neo).
- [ ] T012–T015 Round GC9A01A + CST816S touch diagnostics — **cancelled** (not shipping UI).
- [x] T01x OLED status init when `HAVE_OLED`.
- [x] T01y USB host Neo bring-up on OTG1.

## Phase 2 — Core services

- [x] T020 NVS settings + migration-friendly load/save.
- [x] T021 LittleFS portal + internal neo backups.
- [x] T022 Document path/name validation; SD optional mount.
- [x] T023 `device_status` snapshots for OLED/API.
- [x] T024 Host/unit tests for selected helpers (auth, blank text, …).

## Phase 3 — Touch UI

- [ ] T030–T034 LVGL round shell — **cancelled** for shipping; portal is primary UI.

## Phase 4 — Wi‑Fi onboarding

- [x] T040 Unconfigured-boot detection.
- [x] T041 Setup AP + portal first-run.
- [x] T042 Recovery hotspot behavior.
- [x] T043 Wi‑Fi save / retry / failure messaging.
- [x] T044 Portal password + require auth after setup.
- [x] T045 mDNS from device name.

## Phase 5 — Local portal

- [x] T050 Responsive no-CDN portal.
- [x] T051 Status + Neo connection UI.
- [x] T052 Backup list / upload / download / delete.
- [x] T053 Neo document scan / read / write / backup now / backup all.
- [x] T054 Settings: Wi‑Fi, BLE, auto-backup, cloud, password, factory reset.
- [x] T055 Live keyboard viewer + raw HID; user guide.
- [x] T056 Warn + confirm when leaving keyboard mode for manager ops.

## Phase 6 — BLE HID

- [x] T060 NimBLE + HID keyboard GATT.
- [x] T061 Pairing controls (time-limited).
- [x] T062 Preview / confirm / cancel text send from portal.
- [x] T063 Disconnect / cancel handling.
- [ ] T064 Multi-OS pairing matrix (Windows / Android / iOS / macOS) — **beta validation**.
- [x] T065 Document non-goal: no Neo→BLE key passthrough.

## Phase 7 — Reliability

- [x] T070 Battery monitor when enabled; OLED status.
- [x] T071 Restart + factory reset (NVS + internal neo; SD kept).
- [x] T072 OTA partition present; release packaging script.
- [ ] T073 Full hardware acceptance soak on production BOM — **before 1.0.0**.

## Phase 8 — AlphaSmart Neo

- [x] T080 NeoTools-aligned USB flip + protocol stack.
- [x] T080a UTF‑8 per-document backups (SD or flash).
- [x] T081 HID listen + live monitor in keyboard mode.
- [x] T082 Autobackup on connect; return to keyboard.
- [x] T083 Smart skip blank / duplicate; prune internal flash.
- [x] T084 Optional cloud upload after backup.

## Public beta release checklist

- [x] Specs actualized to Neo2 Buddy behavior.
- [x] BLE scope disclosed (portal → host only).
- [x] Keyboard-mode interruption disclosed (UI + guide).
- [x] Fresh `idf.py build` + `package-release.ps1 -Version 1.0.0`.
- [ ] LICENSE chosen and added at repo root.
- [ ] Hardware soak + BLE host matrix notes beyond release README.
