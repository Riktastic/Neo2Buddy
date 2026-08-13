# Feature Specification: AlphaSmart Neo2 Buddy

**Feature ID**: `001-alphatouch-buddy` (historical folder name)  
**Product name**: AlphaSmart Neo2 Buddy  
**Status**: Implemented — public beta  
**Primary device**: Olimex ESP32-S3-DevKit-Lipo with optional 1.3" OLED (128×64), optional microSD, Li‑Po

## Product outcome

An offline-first USB companion for the AlphaSmart NEO2. The buddy:

- Hosts the Neo over USB (OTG1): keyboard mode (`PID 0xBD04`) and manager/comms mode (`PID 0xBD01`)
- Backs up AlphaWord documents to internal LittleFS and/or microSD
- Serves a local web portal for setup, backups, live keyboard monitoring, BLE text relay, Wi‑Fi, and optional cloud upload
- Shows glanceable status on an optional I2C OLED (web portal is the primary UI)

## Users

- **Owner**: configures Wi‑Fi, portal password, backups, cloud sync, and BLE pairing from the browser.
- **Writer**: types on the Neo (USB keyboard mode); optionally sends portal/backup text to a paired BLE host.
- **Maintainer**: checks battery, network, storage, UART CLI, factory reset, and recovery hotspot.

## User stories

### US1 — First-run onboarding (P1) ✅

As an owner, I power on an unconfigured buddy, join its hotspot, and complete setup (access mode + portal password).

**Acceptance**
1. Unconfigured boot starts a WPA2 hotspot; OLED/portal explain how to connect.
2. Setup requires a portal password (8–63 characters); after setup, portal auth is required by default.
3. Home Wi‑Fi failure leaves recovery hotspot available without exposing secrets.

### US2 — Compact status + web home (P1) ✅

As an owner, I see battery / BLE / Wi‑Fi / Neo state on the OLED (if fitted) and manage everything from the portal.

**Acceptance**
1. OLED is status-only; full actions live in the browser.
2. Portal is responsive; privileged actions require authentication after setup.

### US3 — BLE keyboard passthrough (P1) ✅ — scoped

As a writer, I pair the buddy as a BLE keyboard so Neo keystrokes appear on my phone/PC, and I can optionally paste portal text to the same host.

**Acceptance**
1. Advertising only while pairing is enabled (time-limited) or reconnecting.
2. No keystrokes until a BLE HID host is connected and subscribed.
3. Multi-character send uses preview → confirm → cancelable transfer.
4. **Explicit non-goal:** keys pressed on the Neo are **not** forwarded over Bluetooth. Neo typing is USB-only (live viewer / UART). BLE is portal → paired host only.

### US4 — Local file / backup management (P1) ✅

As an owner, I list, view, download, upload, and delete UTF‑8 backups on SD or internal flash.

**Acceptance**
1. Records show name, size, modified time.
2. Unsafe paths rejected; storage limits enforced.
3. Delete and factory reset require confirmation (factory reset requires password; SD card is not wiped).

### US5 — Local web dashboard (P1) ✅

As an owner, I open the device IP (OLED or serial log; SoftAP is `http://192.168.4.1`) and use status, Neo documents, live keyboard, BLE, Wi‑Fi, cloud sync, and settings offline.

### US6 — Battery and power (P2) ✅ (hardware-dependent)

Battery percent/charging from configured ADC; low-battery warning on OLED/portal when `HAVE_BATTERY`.

### US7 — AlphaSmart Neo USB companion (P1) ✅

As a Neo owner, I connect the Neo by USB, backup/read/write documents, watch live typing in keyboard mode, and return to keyboard mode after backups.

**Acceptance**
1. Neo USB‑B needs **5 V** for USB enumeration (AAs alone are insufficient). See `docs/cable.md`.
2. Keyboard mode: HID listener feeds live portal + UART logs.
3. Manager ops (`ensure_comms`): scan / read / write / backup / applets flip to comms and **interrupt keyboard emulation**.
4. **Backup now / Backup all** return Neo to keyboard mode when finished.
5. **Single Read / Download / Scan** may leave Neo in manager mode until the user taps **Keyboard mode** (RESTART).
6. Portal warns and confirms when leaving keyboard mode for file ops.
7. Smart backup policy: skip blank/whitespace; skip identical existing `.txt`; prune oldest on internal flash when space/count low (SD not pruned by this policy).

### US8 — Removable storage (P1) ✅

Imports prefer SD when present (`HAVE_SDCARD`); otherwise internal `/spiflash/neo`. Atomic writes; incomplete temps ignored.

### US9 — Optional cloud upload (P2) ✅

WebDAV or S3-compatible upload of local backups; local copies never deleted by cloud sync. Optional auto-upload after successful backup.

## Hardware BOM (dev kit)

| Part | SKU | Qty | Notes |
|---|---:|---:|---|
| Olimex ESP32-S3-DevKit-Lipo | 006173 | 1 | OTG1 = Neo USB data; other USB-C = serial |
| PKCELL Li-Po 3.7V 2000mAh JST-PH | 005880 | 1 | On-board charger |
| 1.3" OLED 128×64 I2C | 001709 | 1 | Optional (`HAVE_OLED`) |
| MicroSD adapter w/ level shifter | 000375 | 1 | Optional (`HAVE_SDCARD`) |

## Functional requirements

- FR-001: LittleFS for portal assets + internal backups; NVS for settings/secrets; optional FAT SD mount (never auto-format).
- FR-002: REST API for status, files, Neo ops, Wi‑Fi/BLE, cloud sync, auth, factory reset.
- FR-003: Setup hotspot when unconfigured or recovery; portal via IP (OLED / serial). mDNS / `.local` is not used.
- FR-004: Optional 128×64 OLED status; responsive web UI (no LVGL/touch UI in shipping product).
- FR-005: BLE HID keyboard with **Neo key passthrough** while connected; bonds persist across reboot; portal text send remains optional.
- FR-006: Static portal from LittleFS; user guide at `/user-guide.html`.
- FR-007: Never return Wi‑Fi passwords, portal password hashes, or cloud secrets in API responses.
- FR-008: Operate without SD; expose whether external storage is mounted.
- FR-009: Neo backups as UTF‑8 `.txt` with safe names (`NeoLabel_sNN_…_YYYYMMDD.txt`).
- FR-010: USB host Neo transport: HID keyboard listen; on-demand flip to bulk comms; RESTART back to HID.
- FR-011: Disclose that manager/file operations interrupt keyboard emulation.

## Non-functional requirements

- NFR-001: Portal usable within a few seconds of boot on normal hardware.
- NFR-002: Usable without home Wi‑Fi (AP / recovery hotspot).
- NFR-003: Browser actions show pending / success / error feedback.
- NFR-004: Plain-language copy; warn on keyboard-mode interruption and BLE scope.

## Known limitations (public beta)

1. **BLE ≠ Neo wireless keyboard.** Portal text → paired host only.
2. **Backup / scan / read / write / applet ops interrupt keyboard mode.**
3. Single Read/Download/Scan can leave manager mode until **Keyboard mode**.
4. Neo USB requires 5 V on USB‑B.
5. `privateLive` portal preference is UI-side only (not enforced in firmware).

## Out of scope (current release)

- Neo keystrokes forwarded over BLE.
- Cloud accounts / remote Internet administration of the buddy itself.
- Editing arbitrary binary Neo files in the portal.
- Background BLE typing without preview/confirm.
- Round LVGL touch shell (early plan; not shipping).

## Pinout and wiring

Constants: `firmware/main/include/board_config.h`. Neo USB: `docs/cable.md`.

- OLED I2C: SDA GPIO48, SCL GPIO47, 3.3 V only.
- microSD SPI: CS15, MOSI16, MISO17, CLK18, 3.3 V via level shifter.
- Battery sense: GPIO6 (Olimex divider).
- Neo data: OTG1 (ESP32 native USB). Serial/flash: CH340 USB‑C.
