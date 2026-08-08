# AlphaSmart Neo 2 USB wiring (split power / data)

This document records a **field-tested cable** that connects an AlphaSmart Neo 2 to
the ESP32-S3 buddy while keeping the Neo usable in normal writing mode when the
buddy is unplugged.

## Power requirements

**The Neo 2 USB-B port must receive 5 V for buddy use.** Internal AA batteries can
power the typewriter for normal writing, but they do **not** wake the USB interface
or enter emulation / management mode on their own. Without **VBUS (+5 V) on USB-B**,
the Neo will not listen for the ESP32 host or switch into the mode NeoTools and
this firmware need.

| Power on USB-B | Internal AAs | Normal writing | USB / emulation mode |
| --- | --- | --- | --- |
| **5 V present** (power bank, PC, etc.) | Optional | Yes | Yes — when data host connected |
| **No 5 V** | Installed | Yes (battery power) | **No** — will not respond on USB |

Use any stable **5 V USB supply** on the Neo’s USB-B port that can deliver enough
current — a power bank works well in practice. Internal batteries are optional and
do not replace USB-B VBUS for the buddy workflow.

The ESP32 buddy only needs to be on the **data path** (D+/D− via OTG1). Neo **VBUS**
should come directly from the power bank (or other 5 V source), not from the ESP32.
Use a supply that stays at 5 V under load (Neo + ESP32 from one bank is usually fine).

## Goal

- **5 V on USB-B only (no data host):** Neo 2 runs as a standalone typewriter (normal
  applet mode). Internal batteries alone give the same — writing works, USB does not.
- **5 V on USB-B + ESP32 data connected:** Neo 2 detects a USB host and enters
  transfer / management (emulation) mode so the buddy can import, export, and manage
  files over the NEO protocol.

The Neo **does not need to reboot** when the data path appears. If USB-B already has
5 V and the Neo is writing, plugging in the ESP32 (OTG1) data connection is enough —
it notices the USB host on D+/D− and switches to emulation mode **immediately**.

## Connectors used

Three cable ends were combined into one harness:

| End | Connector | Connects to |
| --- | --- | --- |
| Power source | USB-A male | Power bank (5 V supply) |
| Buddy host | USB-C | ESP32-S3 **OTG1** port (native USB, GPIO19/20) |
| Neo device | USB-B | AlphaSmart Neo 2 device port |

![Custom split cable: USB-B to Neo, USB-C to ESP32 OTG, power bank on USB-A](assets/custom-cable.jpg)

On the Olimex ESP32-S3-DevKit-LiPo, **OTG1** is the native USB port used for Neo
host communication. The other USB-C port is the CH340 serial adapter used for
`idf.py flash monitor`.

## Signal routing

```text
Power bank (USB-A)
    │
    ├─ red  ─────────────────────────► Neo2 USB-B  (VBUS / +5 V)
    │
    └─ white + green + black ──► ESP32 OTG1 (USB-C) ──► Neo2 USB-B (D+/D−, GND)
```

| Wire (USB colour) | Function | Route |
| --- | --- | --- |
| **Red** | VBUS (+5 V) | Power bank USB-A → Neo2 USB-B **directly** |
| **Black** | GND | All grounds tied together (power bank, ESP32, Neo2) |
| **White** | D− | ESP32 OTG1 ↔ Neo2 USB-B |
| **Green** | D+ | ESP32 OTG1 ↔ Neo2 USB-B |

Data lines (white/green) pass through the ESP32 OTG port. Power (red) reaches the
Neo from the power bank without going through the ESP32.

## Observed behaviour

| Setup | Neo2 sees | Result |
| --- | --- | --- |
| Internal AAs only (no USB-B 5 V) | Battery power, no USB VBUS | **Normal mode only** — no USB / emulation |
| 5 V on USB-B, no data host | VBUS, no USB host on D+/D− | **Normal mode** — write on the device |
| 5 V on USB-B + ESP32 data connected | VBUS + USB host (ESP32 OTG) | **Emulation / management mode** — buddy NEO protocol |

**Hot-plug:** The mode change is live. A Neo that is already on and in normal
writing mode switches to transfer / management mode as soon as the ESP32 data
connection is made — no reboot or power cycle required.

Typical workflow:

1. Apply **5 V to Neo USB-B** (e.g. power bank) → Neo powers on; you can write in
   normal mode even without internal batteries.
2. Write as usual, then connect the ESP32 data path (USB-C / OTG1) when you want
   buddy access → Neo switches to emulation / management mode **on the spot**.
3. Use the web portal, UART console (`neo` commands), or BLE features on the buddy.

No PC is required for this path; the ESP32-S3 acts as the USB host.

![Typical bench setup: Neo2, buddy board, and power bank](assets/full-setup-powerbank.jpg)

## Why this works

The Neo 2 USB stack needs **VBUS on USB-B** before it will respond to a host. Internal
AA batteries run the main unit for writing but do **not** substitute for that 5 V on
the USB port.

Once USB-B has 5 V, mode follows the **data pair**:

- **VBUS only:** Neo runs in normal writing mode.
- **VBUS + D+/D− to an ESP32-S3 USB OTG host:** Neo enumerates and enters emulation /
  management mode. This can happen at power-on **or** while already running — the Neo
  watches the data lines and reacts without restarting.

## Safety notes

- **One VBUS source to the Neo.** Do not tie the power bank red wire and an ESP32
  5 V output together unless you have confirmed the board does not back-feed VBUS.
  In this harness, only the power bank feeds Neo VBUS.
- **Common ground is required.** All black wires must be joined; USB data will not
  work reliably otherwise.
- **Current:** Neo 2 plus ESP32-S3 from a single power bank is usually fine; use a
  bank that can deliver sustained 5 V under load.
- **Permanent installs:** For anything beyond a bench cable, add strain relief,
  insulation, and optionally a polyfuse on the Neo VBUS line.

## Firmware expectation

The buddy firmware runs the ESP32-S3 as a **USB host** on OTG1 (`usb_host_neo.c`).
When the Neo is in transfer / management mode and enumerated, import/export,
applet management, and live keyboard bridging can proceed.

If the Neo stays in normal writing mode or is not seen on USB, check:

- Neo **USB-B has 5 V** (power bank or similar — internal batteries alone are not enough).
- ESP32 is powered and running host firmware.
- White/green are connected through OTG1, not the CH340 serial port.
- Ground is shared between all three connectors.

## Related docs

- [Portal showcase](portal-showcase.md) — web UI demo + UART screenshot
- [External microSD wiring](sd-card-wiring.md)
- [Firmware hardware notes](../firmware/main/HARDWARE.md)
- [Board USB pin labels](../firmware/main/include/board_config.h)
