# NEO protocol implementation (`firmware/main/neo`)

This directory implements the AlphaSmart Neo2 **Manager (ASM) protocol** on ESP32-S3
USB OTG. We emulate [NeoTools](https://github.com/alphasmart/neotools) byte-for-byte
where hardware has proven it matters.

**Design history:** [`../../docs/neo-usb-and-backup.md`](../../docs/neo-usb-and-backup.md)  
**USB wiring (user guide):** [`../../../docs/cable.md`](../../../docs/cable.md)

---

## How to read the code (bottom-up)

### 1. USB wire (`usb_host_neo.c` / `.h`)

The Neo has two USB personalities on VID `0x081E`:

| PID | Mode | What we do |
|-----|------|------------|
| `0xBD04` | HID keyboard | Claim interface; optional HID decode; schedule auto-backup |
| `0xBD01` | Bulk comms | Flip target; all ASM bytes go here |
| `0x0100` | Hub stage | Wait for real device |

`usb_host_neo_ensure_comms()` is the only supported entry into protocol mode.

### 2. Framing (`neo_message.c` / `.h`)

Every ASM command after hello/switch is **8 bytes**: cmd, args BE in [1..6], checksum [7].

### 3. Session (`neo_device.c` / `.h`)

Before any 8-byte command:

1. Send `0x01` → read **2-byte** version (not 8!)
2. Send `?reset`
3. Send `?Switch` + applet id → read **8-byte** `"Switched"`

Then `send_command`, `read_extended`, `write_extended`. End with `?reset`.

### 4. Operations

| Module | Role |
|--------|------|
| `neo_applet.c` | LIST/INSTALL/REMOVE/**FETCH** applets via system 0x0000 (NeoTools `applets fetch`) |
| `neo_file.c` | GET/SET attributes, READ_RAW, WRITE_RAW, COMMIT |
| `neo_space.c` | Free ROM/RAM queries |
| `neo_conv.c` | AlphaWord bytes ↔ UTF-8 |
| `neo_import.c` | Save/load `.txt` backups on SD/spiflash |
| `neo_autobackup.c` | Flip → read changed files → RESTART keyboard |
| `neo_usb_hid.c` | Keyboard mode only — not ASM |
| `neo_debug.c` | Ring buffer of protocol trace |

---

## Inline documentation convention

Each `.c` / `.h` file starts with a **plain-language** block explaining:

- What NeoTools does in this layer
- Which command bytes are involved
- Pitfalls we hit on real hardware (hello length, applet id width, flip races)

Read `neo_message.h` and `neo_device.c` (`dialogue_start`) first — everything else builds on that session.

---

## Do not regress

1. Applet IDs are **uint16_t** (`0xA000` AlphaWord).
2. Hello reply is **2 bytes**.
3. Return Neo to keyboard after backup (`usb_host_neo_restart`).
4. Never share one USB bulk transfer for IN and OUT.
5. Flip only from the USB client task.
