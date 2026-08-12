# Host Link Transport (HLT) — draft v0.1

Binary-free framing tunneled through **USB HID boot keyboard** reports (printable ASCII subset).

## Frame format

```text
~|HLT1|<hex_payload>|~
```

| Field | Meaning |
|-------|---------|
| `~|` | Start sentinel (rare in normal prose) |
| `HLT1` | Protocol id + version |
| `<hex_payload>` | UTF-8 message encoded as lowercase hex (two chars per byte) |
| `|~` | End sentinel |

**Max payload:** 512 bytes UTF-8 on ESP32 decoder (`NEO_LINK_HLT_PAYLOAD_MAX`). Neo applet streams prompts up to `NEO_LINK_PROMPT_MAX` (60).

### Message types (first byte of decoded payload)

| Byte | Name | Body |
|------|------|------|
| `0x01` | `CHAT` | UTF-8 user prompt (rest of payload) |
| `0x02` | `PING` | Empty — buddy replies with `PONG` on API |
| `0x7F` | `ABORT` | Cancel partial frame |

Example — prompt `"Hi"`:

```text
~|HLT1|014869|~
```

(`01` = CHAT, `48 69` = `Hi`)

## Neo → ESP32 (implemented in firmware decoder)

1. Applet or test harness emits the frame characters (see `neo_link_protocol_emit_chat`).
2. Each character becomes a USB HID keypress (Neo firmware).
3. Buddy `neo_link_transport_feed_text()` reassembles; on complete frame, parses and stores.

**Collision avoidance:** Decoder ignores traffic until it sees `~|`. While inside a frame, bytes go to the frame buffer only (not live typing buffer).

## ESP32 → Neo (mailbox)

Buddy briefly flips USB to ASM, writes **plain ASCII** into `NeoLinkIn` (512-byte slot per `NEO_LINK_MAILBOX_CAP`), then returns to keyboard mode.

| File | Direction | Name | Index |
|------|-----------|------|-------|
| Inbox | buddy → applet | `NeoLinkIn` | 1 |
| Outbox | applet → buddy | `NeoLinkOut` | 2 |

Encoding follows **NeoTools non-AlphaWord rules**: raw bytes, no softbreak/`0xa7` padding. AlphaWord `neo_conv_import_text_to_neo` is intentionally **not** used.

1. `neo_link_text_to_mailbox()` → printable ASCII
2. `neo_file_create` / `write_raw` with password `"write"` (NeoTools default)
3. Recreate slot if `alloc_size` too small
4. `usb_host_neo_restart()`
5. Applet `FileReadBuffer` on file 1 after USB plug / **Find**

**HID fallback:** applet also `FileWriteBuffer`s the prompt to file 2; buddy `POST /api/v1/link/pull` reads it and runs the LLM.

**Note:** ASM flip interrupts keyboard mode for ~1–3 s per reply. Distraction-free writing in AlphaWord is unaffected unless the user is in the link applet.

## ESP32 → Neo (future)

| Phase | Mechanism |
|-------|-----------|
| v0.3 | Investigate `MSG_USB_*` ack channel from buddy to applet |

## Applet UX (NeoLinkChat)

See `docs/os3k-applet.md` for LCD rows (1–8) and open-path rules.

1. Title + USB status (rows 1–2)
2. Compose: **Enter** → `TextBox` on row 8
3. **Send**: writes NeoLinkOut + streams HLT via `QueueKey`
4. Reply: buddy writes NeoLinkIn — press **Find** to load

## Buddy HTTP (experimental)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/link/status` | Link + LLM readiness / last mailbox error |
| GET | `/api/v1/link/last` | Last decoded prompt + reply |
| GET/PUT | `/api/v1/link/llm` | LLM config (auth required; API key never returned raw) |
| POST | `/api/v1/link/echo` | Body: reply text — stores + delivers to Neo mailbox |
| POST | `/api/v1/link/send` | Push arbitrary text to Neo mailbox |
| POST | `/api/v1/link/pull` | Read applet NeoLinkOut → LLM → NeoLinkIn (HID fallback) |

### LLM config (`PUT /api/v1/link/llm`)

```json
{
  "enabled": true,
  "base_url": "https://api.openai.com/v1",
  "api_key": "sk-...",
  "model": "gpt-5-nano",
  "system": "You are a concise assistant..."
}
```

Also works with OpenAI-compatible local servers, e.g. Ollama: `"base_url": "http://192.168.1.10:11434/v1"`.
