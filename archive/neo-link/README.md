# Neo Link — archived (abandoned)

**Status: permanent dead end for live chat.** Do not restore as a product feature unless Neo OS gains a way for SmartApplets to keep focus while USB keyboard emulation is active.

Archived 2026-08-11 (bring-up pause). **Closed 2026-08-12** after confirming the USB-mode blocker.

## Why it failed

When the Neo is plugged into a USB host (buddy included), the OS forcibly shows:

```text
Attached to PC, emulating keyboard.
```

That screen is **system AlphaWord / USB keyboard mode**. A SmartApplet **cannot** keep focus, paint over it, or run a live chat UI while the cable is connected.

| Direction | While USB keyboard mode (`0xBD04`) |
|-----------|-------------------------------------|
| Neo → buddy (HID keys) | Works (emulate screen owns UI) |
| Buddy → Neo (screen / applet) | **Not possible** without flipping to ASM |
| Applet UI live with USB plugged | **Impossible** — OS steals focus |

ASM mailbox (`0xBD01` flip → write file → restart to keyboard) can move bytes while the cable stays plugged, but it **interrupts** keyboard mode and still does not restore applet UI on top of emulate. That is not “live link.”

Earlier NL 3.9 Adresfout / BetaWise mismatch was a separate bring-up issue; HelloWorld later worked on a compatible OS. The **product** fail is the USB emulate takeover, not the compiler.

## Layout

| Path | Contents |
|------|----------|
| `neo-link-tree/` | Applet, protocol, docs (last working tree, incl. v0.9a attempt) |
| `firmware-buddy/` | ESP32 `neo_link_*.c/h` + embedded `.OS3KApp` |
| `firmware-web/` | `neo-link.html` + `js/neo-link.js` |
| `tools/` | `verify_neo_link_applet.py`, `build-neolinkchat-betawise.ps1` |
| `artifacts/` | Sample / embedded `NeoLinkChat.OS3KApp` copies removed from live tree |

## Firmware state

- `CONFIG_BUDDY_NEO_LINK` defaults **off** and should stay off.
- Portal Neo Link UI is not shipped.
- No `NeoLinkChat.OS3KApp` under `samples/applets/` or `firmware/main/neo/embedded/`.

## Do not restore unless

1. Neo OS allows applets to remain focused during USB attach, **or**
2. Product accepts offline-only compose + buddy-side UI (no live Neo chat), **or**
3. A new host→Neo channel exists that does not require leaving keyboard mode (see below).

## Future: wireless / Beamer-like link

USB is the wrong pipe for live applet chat. A more promising direction is **non-USB radio**, the same class of path Beamer uses:

| Idea | Notes |
|------|--------|
| **Beamer (IR)** | Stock SmartApplet that keeps UI focus while transferring to another Neo. Proof that applets can own the screen during a link session. Protocol / syscalls not documented in this repo — would need dump/disassembly or AlphaSmart docs. |
| **Neo2 wireless + Renaissance receiver** | Classroom RF path. BetaWise `os3k.h` already stubs `SYS_INT_WIRELESS_DISPLAY_STATUS` and `SYS_INT_WIRELESS_TURN_ON`; pairing, framing, and buddy-side receiver integration are unknown. |
| **Buddy role** | ESP32 (or a dedicated dongle) as the far end of Beamer-style / Renaissance RF, carrying small chat frames while the Neo applet stays focused — no USB emulate takeover. |

**Status:** research only. Do not schedule until Beamer (or wireless syscalls) are understood well enough to send/receive a few dozen bytes reliably. Offline applets (dice, timers, cheat sheets) should not wait on this.
