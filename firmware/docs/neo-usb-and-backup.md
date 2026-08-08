# Neo USB transport & backup design

This document records **what we tried**, **what failed**, and **why the current
approach is the one that works** for AlphaSmart Neo2 + ESP32-S3 (Olimex OTG1).

Companion wiring notes: [`docs/neo2-usb-wiring.md`](../../docs/neo2-usb-wiring.md).  
Code map: [`main/neo/README.md`](../main/neo/README.md).

---

## 1. Mental model: two USB personalities

The Neo2 exposes **one physical USB-B port** but **two product IDs** under
vendor `0x081E` (Renaissance Learning / AlphaSmart):

| Mode | PID | Role |
|------|-----|------|
| Keyboard (HID) | `0xBD04` | Default when plugged into a host — types as a USB keyboard |
| Comms (bulk) | `0xBD01` | Manager / NeoTools protocol — 8-byte framed messages + block I/O |
| Hub stage (rare) | `0x0100` | Transient re-enumeration; wait, do not treat as ready |

**Insight:** You cannot talk NeoTools protocol while the device is still HID.
Every backup, file list, or applet op must go through **HID → flip → BD01 →
bulk → work → RESTART → BD04**. Leaving the Neo in manager mode after a
portal action is a bad UX (user expects to keep typing).

### Backup content policy

- Skip documents that are empty or only whitespace (spaces / tabs / newlines).
- Skip when any existing backup already has identical UTF-8 bytes (not only today's dated path).
- Before writing on internal flash, prune oldest `.txt` backups if free space or file count exceeds limits (SD card is not wiped by factory reset; pruning applies to the active backup directory).

Reference implementation: upstream **[NeoTools](https://github.com/lykahb/neotools)**.

---

## 2. What we tried on the USB host path

### Failed / abandoned approaches

| Attempt | Symptom | Why it failed |
|---------|---------|---------------|
| Simulated `usb_host_neo` stub always “connected” | Portal showed Ready with no hardware | Fine for UI scaffolding; useless for real Neo |
| Assume consumer USB-C→A cable + OTG alone | `0 devices` forever on OTG1 | Neo USB-B needs **VBUS 5 V**; AAs alone do not wake the USB interface |
| Power Neo from ESP32 VBUS | Brownouts / flaky enum | Better: **split power** — bank→Neo VBUS, ESP only on D+/D− (see wiring doc) |
| Drop `NEW_DEV` while client busy | Neo appears then vanishes | Race: must **queue** `pending_addr`, never silently discard |
| Single shared bulk transfer object for IN+OUT | Deadlocks / `NOT_FINISHED` | Separate IN and OUT `usb_transfer_t`; drain **only** the direction about to submit |
| Hello handshake reading 8 bytes | Protocol hang | NeoTools hello reply is **2 bytes** (protocol version) |
| Truncating applet ID to `uint8_t` | AlphaWord `0xA000` became `0` | Applet IDs are **`uint16_t`** end-to-end |
| Verbose `ensure_comms` success spam | Serial unusable during normal use | Ring always records; serial/log_buffer only with `--debug` / `neo debug on` |
| Auto-backup on every USB event without cooldown | Flip storm after RESTART re-enum | **2 min cooldown** + only trigger from **HID connect** |

### What works best (current design)

1. **Probe already-connected devices at client start** — `NEW_DEV` can be missed if Neo was plugged before the client registered.
2. **NeoTools flip sequence** on HID: `SET_CONFIGURATION(1)` then vendor control outs with magic bytes `0xE0…0xE4` (`bmRequestType=0x21`, `bRequest=9`, `wValue=0x0200`).
3. **~4 s flip wait + retry** when BD01 does not appear (matches NeoTools).
4. **Settle delays** after BD01 (`NEO_USB_COMMS_SETTLE_MS` / `POST_FLIP_MS`) — ASM firmware needs a beat before bulk I/O is stable.
5. **`usb_host_neo_ensure_comms()`** as the single entry for “make protocol usable”:
   - already ready → return
   - flipping → wait
   - BD01 on bus but not opened → reopen
   - keyboard → request flip on the USB client task (thread-safe via action queue)
6. **`usb_host_neo_restart()`** = ensure comms → `REQUEST_RESTART` → invalidate session → wait for HID again. This is how we **return typing mode** after backups.
7. **Comms on demand** — claiming HID does **not** auto-flip forever; flip happens when a manager op needs it (and auto-backup). That keeps plug-in snappy and avoids surprising mode changes when the setting is off.

---

## 3. Auto-backup on connect

### Goal

When a Neo plugs in as keyboard and the setting is on:

1. Settle briefly (USB stack / HID claim settle)
2. Flip to comms
3. Backup **changed** AlphaWord files (`0xA000`, indices 1–8)
4. RESTART back to keyboard

### Why “changed files only” for auto path

| Approach | Pros | Cons |
|----------|------|------|
| Backup all files every plug | Simple | Slow, wears flash, noisy when user plugs/unplugs often |
| Backup only if content differs from last local file | Fast common case | Needs stable path naming + content compare |
| Hash in NVS per file | Avoids reread of disk | Extra state; clock/label changes complicate identity |

**Chosen:** read Neo file → UTF-8 convert → compare to existing
`{label}_s{NN}_{name}_{YYYYMMDD}.txt` via `neo_import_file_matches()`. Skip
unchanged. Empty/`alloc_size==0` skipped. Cap raw read at 256 KiB.

Full “backup everything” remains an **explicit** action (`read-all` / CLI /
Python `backup_all`) and also ends with RESTART to keyboard.

### Why async for web, sync for CLI

- Web HTTP handlers must not block the httpd task for minutes →
  `neo_autobackup_start_async()` + progress fields on GET status/autobackup.
- UART CLI can block the console task → `neo_autobackup_run_now()`.
- Connect trigger always uses the **background task** so USB client task is
  never stuck in long file I/O.

### Cooldown (2 minutes)

After RESTART the Neo re-enumerates as HID, which would otherwise schedule
another auto-backup immediately. Cooldown skips that loop. Explicit
`now` / portal Backup now **force** and ignore cooldown + setting check for
“enabled” (force still requires not already busy).

### Progress phases

`idle → settle → comms → file (current/total) → restart → done`

Exposed to portal and Python client so the UI can show “Backing up file 2/8…”.

---

## 4. Backup filenames

Format:

```text
{label}_s{NN}_{fileName}_{YYYYMMDD}.txt
```

| Piece | Source | Why |
|-------|--------|-----|
| `label` | `neo_label` if set, else Buddy `device_name` | Neo has **no usable MAC/serial** over the protocol for naming |
| `sNN` | AlphaWord file index 1–8 | Stable even if display name is empty/duplicate |
| `fileName` | Sanitized Neo document name | Human readable |
| `YYYYMMDD` | `time()` after SNTP (STA) | Day-level versions; `nodate` until clock is valid |

Tried and rejected: using ESP Wi-Fi MAC as “device id” (identifies the buddy
board, not which Neo), and inventing a fake Neo serial.

Storage: `/sdcard/neo` when SD mounted, else `/spiflash/neo`. Writes are
atomic (`.tmp` + `fsync` + `rename`).

---

## 5. Debug discipline

| Channel | Always? | When verbose |
|---------|---------|--------------|
| In-RAM ring (`neo debug` / `/api/v1/neo/debug`) | Yes | — |
| `ESP_LOG` / portal log_buffer | Failures (WARN+) | All `neo_debug_event` / xfer traces |

USB monitor logs **state changes** only, not a heartbeat every N seconds of
identical status. That keeps field serial useful while still allowing deep
traces with `--debug` on any `neo` CLI command.

---

## 6. API / CLI surface (backup-centric)

| Path / command | Behavior |
|----------------|----------|
| Setting `auto_backup_on_connect` | Enable connect-triggered path |
| Setting `neo_label` | Filename label |
| `POST /api/v1/neo/autobackup` | Async changed backup → keyboard |
| `GET /api/v1/neo/autobackup` | Progress + last_result |
| `POST …/applets/0xA000/files/read-all` | All non-empty → keyboard |
| `POST /api/v1/neo/restart` | Return to keyboard only |
| `neo autobackup on\|off\|now\|status` | UART (human text; `--json` optional) |
| Python `neo2buddy_wrapper` package | Remote login + backup_and_pull |

---

## 7. Do not regress these invariants

1. **Always return to keyboard** after automated or “Backup all” portal flows
   unless the caller explicitly wants manager mode.
2. **Never flip from the wrong task** without going through the USB client
   action queue (`NEO_USB_ACTION_FLIP`).
3. **Never share one USB transfer** for IN and OUT.
4. **Applet IDs are 16-bit**; AlphaWord is `0xA000`.
5. **Hello reply is 2 bytes**, not a full 8-byte Neo message.
6. Auto-backup trigger = **HID open success**, not every `NEW_DEV` or monitor tick.
