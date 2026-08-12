# Research: BetaWise applet + ESP32 LLM proxy

## Goal

A custom Neo2 SmartApplet for chat-style interaction where the **ESP32 Buddy** is a Wi‑Fi proxy to an LLM, without turning the Neo into a “smart device” for normal writing.

## What we know (code + BetaWise + NeoTools)

### USB on Neo2 Buddy

| Mode | PID | Direction | Used for |
|------|-----|-----------|----------|
| Keyboard | `0xBD04` | Neo → host only | Live typing, HLT carrier |
| Manager (ASM) | `0xBD01` | Host ↔ Neo files | Backup, **mailbox In/Out** |

### NeoTools file write rules (critical)

From [neotools](https://github.com/lykahb/neotools) / buddy `neo_file_*`:

1. **Charmap import only for AlphaWord (`0xA000`)** — custom applets get raw bytes.
2. Create = SET_ATTR → **COMMIT** → WRITE_RAW; password default `"write"`.
3. New files use unbound `file_space` (`0xff`).
4. Overwrite does not auto-grow `alloc_size` — recreate if reply outgrows slot.

Neo Link mailbox therefore writes **plain ASCII padded to 512**, never `neo_conv_import_text_to_neo`.

### Dual uplink

| Path | Mechanism |
|------|-----------|
| Primary | HLT over HID (`QueueKey` + `SetKeyModifiers`) |
| Fallback | Applet `FileWriteBuffer` → `NeoLinkOut`; buddy `POST /api/v1/link/pull` |

### BetaWise / OS3K (from [os3k.h](https://github.com/isotherm/betawise/blob/master/os3k/os3k.h))

**Documented in depth:** `docs/os3k-applet.md` (coordinates, RAM, open path, build).

Summary:

| API / message | Notes |
|---------------|-------|
| `PutStringCentered(1,…)` / `SetCursor(row≥1,col≥1)` | **1-based** LCD — see `os3k-applet.md` |
| `MSG_USB_PLUG` / `MSG_USB_UNPLUG` | Detect buddy cable |
| `MSG_SETFOCUS` | Paint only; no `FileOpen` on open |
| `MSG_KEY` / `TextBox()` | Compose on row 8, col 1 |
| `QueueKey()` | Stream HLT (no 1 KB frame buffer on Neo) |
| `FileWriteBuffer` / `FileReadBuffer` | NeoLinkOut / NeoLinkIn |
| `KEY_SEND` | Send key — test on Neo2 + USB |
| `MSG_USB_UNK_*` | Undocumented — debug only |

BetaWise itself lists as WIP: timers, `ProcessMessage` statuses, and **non-message keyboard functions** — expect rough edges.

### Does typing in a SmartApplet reach USB HID?

**Open question #1.** AlphaWord in keyboard mode clearly emits HID reports (buddy live view). Whether **SmartApplet focus** still forwards key events to the USB stack is not documented in this repo. The NeoLinkChat prototype assumes we can emit frames via `QueueKey()` or explicit send — **validate early.**

If SmartApplet keys never reach USB, fallbacks are:

1. **Implemented:** applet `FileWriteBuffer` → `NeoLinkOut`; buddy `POST /api/v1/link/pull`
2. Contribute BetaWise/OS3K research if a hidden USB write API exists.

## Proposed transport: HLT (Host Link Transport)

See `protocol.md`. Summary:

- **Neo → ESP32:** printable framing over HID (`~|HLT1|hexpayload|~`)
- **ESP32 → Neo (phase 1):** reply on buddy web API / logs
- **ESP32 → Neo (phase 2):** ASM mailbox `NeoLinkIn` — **implemented** (`neo_link_mailbox.c`)
- **ESP32 → Neo (phase 3):** if `QueueKey` from a background ASM-written trigger is ever possible (speculative)

## LLM proxy on ESP32

**Implemented** in `firmware/main/neo/neo_link_llm.c` (gated by `CONFIG_BUDDY_NEO_LINK`):

1. HLT `CHAT` → async task → OpenAI-compatible `POST …/chat/completions`
2. Reply stored for `GET /api/v1/link/last` and written to Neo mailbox
3. If LLM not configured / Wi‑Fi down / API error → echo fallback
4. Config via authenticated `PUT /api/v1/link/llm` (NVS namespace `neo_link`)

**Security:** API keys live on the buddy (NVS), never on the Neo. Config GET/PUT require portal Bearer auth. Factory reset erases `neo_link`.

## Hardware test plan

1. **HID from applet:** Run NeoLinkChat, USB to buddy, press keys — does `/api/keyboard/recent` update?
2. **QueueKey → HID:** Applet calls `QueueKey` for `a`,`b`,`c` — does buddy see `abc`?
3. **HLT decode:** Applet sends `~|HLT1|48656c6c6f|~` (`Hello`) — does `GET /api/v1/link/last` show `Hello`?
4. **Send key:** Press physical Send in applet — any HID or ASM side effect?
5. **Mailbox pull:** Write Out via Send, `POST /api/v1/link/pull` — does LLM reply land in NeoLinkIn?
6. **USB messages:** Log all `MSG_USB_*` params in a debug applet.

## Distraction-free alignment

- Link mode should be **opt-in** inside the applet (explicit “Send to buddy”).
- Normal AlphaWord writing unchanged.
- No background ASM polling while user writes in AlphaWord.

## Related projects

- [BetaWise](https://github.com/isotherm/betawise) — applet toolchain
- [NeoTools](https://github.com/lykahb/neotools) — USB manager protocol (buddy already uses)
- Neo2 Buddy — HID + ASM implementation in `firmware/main/neo/`
