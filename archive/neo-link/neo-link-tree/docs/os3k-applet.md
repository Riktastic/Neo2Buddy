# OS3K / BetaWise applet constraints (Neo Link)

**Requirements (MUST/SHALL):** [`requirements.md`](requirements.md)  
Hard-won implementation rules for **Neo2 SmartApplets** built with [isotherm/betawise](https://github.com/isotherm/betawise). Apply to every Neo Link applet change.

## Coordinate system (critical)

| API | Convention | Example |
|-----|------------|---------|
| `PutStringCentered(row, …)` | **1-based row** | `PutStringCentered(1, "hello")` — [HelloWorld.c](https://github.com/isotherm/betawise/blob/master/applets/HelloWorld/HelloWorld.c) |
| `SetCursor(row, col, …)` | **1-based** | DebugTool: `SetCursor(row, 1, …)` |
| `ClearRowCols(row, col_first, col_last)` | **1-based** | DebugTool: `ClearRowCols(4, 1, 40)` |

**Never use row 0 or col 0.** BetaWise `SetCursor` does `(col - 1) * width`. Column 0 wraps to a huge offset and corrupts memory near **`0x5C68`** (internal cursor struct). Neo shows:

```text
Adresfout bij toegang: 0x5cxxxx
```

Fault addresses like `0x5c1365`, `0x5c2563`, `0x5c36e3` are consistent with this bug class.

### Neo2 16px font layout (Neo Link Chat)

| Row | Use |
|-----|-----|
| 1 | Title |
| 2 | USB status |
| 3–6 | Reply viewport (4 lines) |
| 7 | Hint |
| 8 | Status / TextBox input |

Cols **1–40** only (`NEO_LINK_LCD_COLS`). Use `neo_link_os3k.h` row constants — never literals `0` or `9+`.

## Hard limits (do not exceed)

All values in **`neo-link/protocol/neo_link_limits.h`**. Build fails if violated.

| Limit | Value | What breaks if exceeded |
|-------|-------|-------------------------|
| `NEO_LINK_APPLET_RAM_BUDGET` | 1536 B | Install may fail; BSS may corrupt OS3K (`0x5Cxxxx` Adresfout) |
| `NEO_LINK_APPLET_RAM_LEGACY_MAX` | 800 B | Buddy auto-replaces; above this historically crashed on open |
| `NEO_LINK_APPLET_MAX_LINK_OBJECTS` | 2 | Linking `neo_link_protocol.c` → ~5612 B RAM |
| `NEO_LINK_APPLET_STACK_BUF_MAX` | 64 B | Large stack arrays overflow 68k stack |
| `NEO_LINK_LCD_ROWS` / `COLS` | 8 / 40 | Row/col `0` → cursor math bug near `0x5C68` |
| `NEO_LINK_PROMPT_MAX` | 60 | TextBox + outbox |
| `NEO_LINK_REPLY_MAX` | 200 | `FileReadBuffer` + LCD wrap |
| `NEO_LINK_OUTBOX_CAP` | 128 | Applet BSS (`neo_link_emit.c`) |
| `NEO_LINK_MAILBOX_CAP` | 512 | Buddy ASM slot size |
| `NEO_LINK_APPLET_FILE_USAGE` | 1024 | Header `fileUsage` (2× mailbox) |
| `NEO_LINK_HLT_PAYLOAD_MAX` | 512 | ESP decoder only — **not** on Neo |

**Enforcement:** `neo_link_applet_guard.h` (compile-time), `tools/verify_neo_link_applet.py` (post-link), `build-docker.ps1` (CI gate).

## RAM budget (critical)

Inspect built `.OS3KApp` header: `ramUsage` at offset **8** (big-endian u32).

| Applet | Typical ramUsage |
|--------|------------------|
| Control Panel (stock) | ~416 B |
| Neo Link Chat v0.8c | ~608 B |
| Neo Link Chat v0.8a/b | ~1112 B |
| Neo Link Chat v0.5–0.7 (linked protocol) | **5612 B** → crashes on open |

**Rules:**

1. **Do not link `neo_link_protocol.c` or `neo_link_snprintf.c` into the Neo applet.** Their static buffers (`s_hex[1024]`, frame buffers) live in BSS and blow `ramUsage`.
2. **No large stack buffers** in applet code (`char buf[512]` in a function). Neo 68k stack is tiny.
3. **Prefer one `struct` of state** over many scattered globals (betawise WIP note).
4. **HID uplink:** stream `~|HLT1|01<hex>|~` via `QueueKey` per character (`neo_link_emit.c`), never `snprintf` a full frame on stack.
5. Keep **`ramUsage + fileUsage` sane** — header `fileUsage` is separate but install checks `ram_size + file_space`.

Target: **`ramUsage < NEO_LINK_APPLET_RAM_BUDGET`** (see `neo_link_limits.h`).

## Open / focus path

On `MSG_SETFOCUS`:

1. **Paint UI only** (HelloWorld pattern: `PutStringCentered`, no `ClearScreen` — framework already cleared).
2. **No `wrap_reply`, viewport, or `FileOpen`** on `MSG_SETFOCUS`.
3. Empty mailbox slots on first open have caused faults when read too early.

`BwProcessMessage` already calls `_OS3K_ClearScreen()` before your `ProcessMessage` on `MSG_SETFOCUS`.

## File mailboxes

| Index | Name | Writer | Reader |
|-------|------|--------|--------|
| 1 | NeoLinkIn | Buddy ASM | Applet `FileReadBuffer` |
| 2 | NeoLinkOut | Applet `FileWriteBuffer` | Buddy `/api/v1/link/pull` |

- Raw bytes only (not AlphaWord charmap). See `neo-link/docs/protocol.md`.
- `FileReadBuffer(buf, NEO_LINK_REPLY_MAX)` — length must match buffer, not full ASM slot if slot is larger.
- Buddy writes **`NEO_LINK_MAILBOX_CAP`**-byte fixed slots; content truncated to **`NEO_LINK_REPLY_MAX`**.

## Shared limits

All cross-boundary sizes live in **`neo-link/protocol/neo_link_limits.h`**.

Firmware includes it via CMake `INCLUDE_DIRS`. Applet copies it in `build-docker.ps1`.

After changing limits: rebuild applet **and** firmware; reinstall applet on Neo.

## Build (Docker / CRLF)

- GNU Make breaks on **CRLF** (`No rule to make target 'all\r'`).
- `build-docker.ps1` writes **LF** files and runs `docker-build.sh` inside the mount.
- Applet link set: **`NeoLinkChat.o` + `neo_link_emit.o` only** (+ libos3k).

## Install / test

1. Flash buddy firmware (embedded `.OS3KApp`).
2. Plug Neo into buddy USB — **auto-install/replace** runs if the on-device applet is missing, outdated, or `ramUsage` &gt; 800 (legacy crash builds).
3. Portal → **Neo Link** should show `applet_up_to_date: true` and installed version **0.8.c**.
4. Power-cycle Neo: **Left Shift + Tab** → **Neo Link Chat** — row 3 must show **0.8c**.
5. Path 2 (file outbox) works without HID; Path 3 needs QueueKey → buddy keyboard API.

## References

- [betawise](https://github.com/isotherm/betawise) — `os3k/os3k.c`, `applets/HelloWorld`, `applets/DebugTool`
- [neotools](https://github.com/lykahb/neotools) — ASM file write, applet install
- [lykahb/alphasmart-research](https://github.com/lykahb/alphasmart-research) — firmware / AlphaWord internals
