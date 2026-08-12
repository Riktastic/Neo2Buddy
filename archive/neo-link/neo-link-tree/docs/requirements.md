# Neo Link — requirements

What Neo Link **must** do, and what SmartApplets **must not** exceed.  
Limits are defined in [`protocol/neo_link_limits.h`](../protocol/neo_link_limits.h).  
Platform rules: [`os3k-applet.md`](os3k-applet.md). Protocol: [`protocol.md`](protocol.md).

---

## 1. Product (Neo Link Chat applet)

| ID | Requirement |
|----|-------------|
| **P-1** | The applet SHALL be a separate SmartApplet with id **`0xA1C0`** (`NEO_LINK_APPLET_ID`). It SHALL NOT use AlphaWord files (`0xA000`). |
| **P-2** | The user SHALL be able to compose a short question on the Neo and send it to the buddy. |
| **P-3** | The user SHALL be able to read an LLM reply on the Neo LCD (wrapped text, scroll Up/Down). |
| **P-4** | **Find** SHALL reload `NeoLinkIn` (buddy → Neo mailbox). |
| **P-5** | **Enter / Send** SHALL open compose (`TextBox`) and then send when non-empty. |
| **P-6** | Sending SHALL write the prompt to **`NeoLinkOut`** (file 2) and SHALL stream an HLT `CHAT` frame over HID via `QueueKey` (character by character). |
| **P-7** | Path 2 (file outbox + buddy `POST /api/v1/link/pull`) SHALL work even when HID streaming fails. |
| **P-8** | Opening the applet SHALL NOT crash the Neo (no **Adresfout** on `MSG_SETFOCUS`). |

---

## 2. Buddy firmware

| ID | Requirement |
|----|-------------|
| **B-1** | Firmware SHALL embed the bundled `NeoLinkChat.OS3KApp` built from this repo. |
| **B-2** | On Neo USB connect, firmware SHALL compare the installed `0xA1C0` applet to the bundle and SHALL **replace** it if missing, version older than bundled, or `ramUsage > NEO_LINK_APPLET_RAM_LEGACY_MAX` (800 B). |
| **B-3** | Buddy SHALL decode HLT frames from Neo HID in `neo_link_protocol.c` (ESP32 only — never linked into the Neo applet). |
| **B-4** | Buddy SHALL write LLM replies to **`NeoLinkIn`** as plain ASCII in a fixed **`NEO_LINK_MAILBOX_CAP`** (512 B) ASM slot — not AlphaWord charmap. |
| **B-5** | Buddy SHALL read **`NeoLinkOut`** via ASM for `/api/v1/link/pull`. |
| **B-6** | Portal `/api/v1/link/status` SHALL report bundled vs installed applet version and whether an update is needed. |

---

## 3. OS3K platform (all Neo Link applets)

| ID | Requirement |
|----|-------------|
| **O-1** | LCD row and column arguments to `SetCursor`, `ClearRowCols`, `PutStringCentered`, and `TextBox` SHALL be **1-based**. Row and col **0 are forbidden** (causes `Adresfout` near `0x5Cxxxx`). |
| **O-2** | Neo2 with 16px font: rows **1–8**, cols **1–40** (`NEO_LINK_LCD_ROWS`, `NEO_LINK_LCD_COLS`). Layout constants SHALL come from [`neo_link_os3k.h`](../applet/neo_link_os3k.h). |
| **O-3** | On **`MSG_SETFOCUS`**, the applet SHALL only paint the UI (HelloWorld pattern). It SHALL NOT call `FileOpen`, `FileReadBuffer`, `wrap_reply`, or heavy viewport logic on open. |
| **O-4** | `BwProcessMessage` already clears the screen before `MSG_SETFOCUS`; the applet SHALL NOT call `ClearScreen()` on open unless redrawing after user action. |
| **O-5** | Applet mailboxes SHALL use raw bytes. AlphaWord import rules (`0xa7` softbreak, charmap) SHALL NOT apply. |

---

## 4. Memory and link (hard ceilings)

Values in **`neo_link_limits.h`**. Build **MUST** fail if exceeded (`build-docker.ps1`, `tools/verify_neo_link_applet.py`).

| ID | Limit | Value | Requirement |
|----|-------|-------|-------------|
| **M-1** | `NEO_LINK_APPLET_RAM_BUDGET` | 1536 B | Built `ramUsage` (header offset 8) SHALL be ≤ this. |
| **M-2** | `NEO_LINK_APPLET_RAM_LEGACY_MAX` | 800 B | Target for shipping; buddy auto-replaces above this. v0.8c ≈ 608 B. |
| **M-3** | `NEO_LINK_APPLET_MAX_LINK_OBJECTS` | 2 | Applet link set SHALL be **`NeoLinkChat.o` + `neo_link_emit.o`** only (+ `libos3k`). |
| **M-4** | Forbidden on Neo | — | **`neo_link_protocol.c`** and **`neo_link_snprintf.c`** SHALL NOT be linked into the applet (~5612 B `ramUsage`, open crash). |
| **M-5** | `NEO_LINK_APPLET_STACK_BUF_MAX` | 64 B | No `char buf[N]` with N ≥ 256 on the stack; use `static` BSS or stream. |
| **M-6** | `NEO_LINK_PROMPT_MAX` | 60 | Max `TextBox` length and outbox payload. |
| **M-7** | `NEO_LINK_REPLY_MAX` | 200 | Max `FileReadBuffer` length and reply buffer. |
| **M-8** | `NEO_LINK_OUTBOX_CAP` | 128 | `FileWriteBuffer` slot size in applet BSS. |
| **M-9** | `NEO_LINK_MAILBOX_CAP` | 512 | Buddy ASM slot size for In/Out. |
| **M-10** | `NEO_LINK_APPLET_FILE_USAGE` | 1024 | Applet header `fileUsage`; SHALL be ≥ 2 × mailbox cap. |
| **M-11** | `fileCount` | 2 | Files 1 = NeoLinkIn, 2 = NeoLinkOut. |
| **M-12** | `NEO_LINK_HLT_PAYLOAD_MAX` | 512 | ESP32 decoder only; Neo streams HID per character. |

---

## 5. Build and release

| ID | Requirement |
|----|-------------|
| **R-1** | Applet SHALL be built with [`applet/NeoLinkChat/build-docker.ps1`](../applet/NeoLinkChat/build-docker.ps1) (Docker + BetaWise m68k). |
| **R-2** | Build scripts SHALL use **LF** line endings for Make/shell (CRLF breaks GNU make). |
| **R-3** | After link, `tools/verify_neo_link_applet.py` SHALL pass (RAM, version, fileCount, no col-0 in sources, Makefile object count). |
| **R-4** | Output SHALL be copied to `firmware/main/neo/embedded/NeoLinkChat.OS3KApp` before firmware build. |
| **R-5** | Applet version in header SHALL match `NEO_LINK_APPLET_VERSION_*` in `neo_link_limits.h`. Current: **0.8.c**. |
| **R-6** | Changing any limit in `neo_link_limits.h` SHALL require rebuilding **both** applet and firmware. |

---

## 6. Acceptance (how we know it works)

| ID | Check |
|----|--------|
| **A-1** | Flash buddy firmware; plug Neo — logs or portal show applet sync / `applet_up_to_date: true`. |
| **A-2** | Open **Neo Link Chat** (Left Shift+Tab) — **no Adresfout**; row 3 shows **`0.8c`**. |
| **A-3** | Portal **Send test reply → Neo** + **Find** on Neo shows text. |
| **A-4** | Type prompt → **Send** → portal **Fetch prompt from Neo & reply** → **Find** shows LLM reply. |
| **A-5** | `verify_neo_link_applet.py` exits 0 after `build-docker.ps1`. |

See [`validation.md`](validation.md) for the full hardware checklist.

---

## 7. Enforcement map

| Layer | Artifact |
|-------|----------|
| Constants | `neo-link/protocol/neo_link_limits.h` |
| Compile-time | `neo-link/applet/neo_link_applet_guard.h`, `_Static_assert` in `NeoLinkChat.c` |
| Post-link | `tools/verify_neo_link_applet.py` |
| Build gate | `neo-link/applet/NeoLinkChat/build-docker.ps1` |
| Runtime | `firmware/main/neo/neo_link_applet.c` (auto-install / version check) |

---

## 8. Out of scope (current)

- AlphaWord document import/export through Neo Link.
- Neo applet receiving LLM replies only via HID (mailbox is primary).
- Multiple Neo Link applets or ids other than `0xA1C0`.
- Wireless-only Neo models without USB to buddy.
