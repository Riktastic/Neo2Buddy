# Neo Link — SmartApplet ↔ ESP32 ↔ GPT

> **ABANDONED.** See [`../README.md`](../README.md). Live chat UI is not possible
> while USB “emulating keyboard” mode owns the Neo screen.

Experimental **Host Link Transport (HLT)** plus **NeoTools-style file mailboxes** so a Neo2 can ask an LLM through the buddy.

## Status

| Piece | State |
|-------|--------|
| **Requirements** | **`docs/requirements.md`** |
| Protocol | `docs/protocol.md` |
| **OS3K applet rules** | **`docs/os3k-applet.md`** |
| Research | `docs/research.md` |
| Shared limits | `protocol/neo_link_limits.h` |
| Applet guards | `applet/neo_link_applet_guard.h` |
| Verify script | `tools/verify_neo_link_applet.py` |
| Framing codec (ESP only) | `protocol/neo_link_protocol.c` |
| Applet | `applet/NeoLinkChat/` + **`NeoLinkChat.OS3KApp`** (2 files: In + Out) |
| HID decoder | `neo_link_transport.c` |
| Mailbox (ASM) | `neo_link_mailbox.c` — **raw bytes**, not AlphaWord charmap |
| LLM proxy | `neo_link_llm.c` — OpenAI-compatible `/chat/completions` |

## Architecture (NeoTools-inspired)

```
Neo applet                    ESP32 Buddy                         Cloud / Ollama
─────────                    ───────────                         ──────────────
Type prompt
  ├─ FileWriteBuffer → NeoLinkOut ──ASM──► POST /link/pull ──┐
  └─ QueueKey HLT frame ──HID──► decode CHAT ────────────────┤
                                                             ▼
                                                    chat/completions
                                                             │
NeoLinkIn ◄── ASM WRITE_RAW (plain ASCII) ◄── mailbox ◄──────┘
  FileReadBuffer → LCD
```

**Key NeoTools lesson:** charmap import is **AlphaWord-only** (`0xA000`). Custom applet files get raw bytes. We do **not** pad with `0xa7` or inject softbreaks into the mailbox.

## Enable firmware

```text
idf.py menuconfig → Neo2 Buddy Configuration → Enable Neo Link HID transport
idf.py build flash
```

## Configure GPT

Prefer the portal page **`/neo-link.html`** (nav: Neo Link): enable, URL, key, model, **Test API**, Path 1/2 buttons.

Or via UART (115200, same portal password):

```text
login <password>
link llm set enabled on
link llm set base_url https://api.openai.com/v1
link llm set api_key sk-...
link llm set model gpt-5-nano
link llm test
link llm
```

Or via API:

```bash
curl -X PUT http://<buddy>/api/v1/link/llm \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"enabled":true,"base_url":"https://api.openai.com/v1","api_key":"sk-...","model":"gpt-5-nano","max_tokens":450,"max_rpm":6,"context_turns":2}'
```

Modern OpenAI models (`gpt-5*`, `o1`/`o3`/`o4`) automatically use `max_completion_tokens`, omit `temperature`, and send `reasoning_effort=minimal` with a larger completion budget so short Neo replies are not eaten by hidden reasoning tokens.
Ollama: `"base_url":"http://192.168.x.x:11434/v1"` (key optional).

**Separate from AlphaWord** — mailboxes are only on applet `0xA1C0`. Hardware checklist: `docs/validation.md`.

## HTTP API

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/link/status` | Link + LLM readiness |
| GET | `/api/v1/link/last` | Last prompt / reply |
| GET/PUT | `/api/v1/link/llm` | LLM config (auth) |
| POST | `/api/v1/link/llm/test` | Call API without writing Neo |
| POST | `/api/v1/link/llm/clear-context` | Drop prior turns |
| POST | `/api/v1/link/send` | Push text → NeoLinkIn |
| POST | `/api/v1/link/pull` | Read NeoLinkOut → LLM → NeoLinkIn |
| POST | `/api/v1/link/echo` | Manual reply → NeoLinkIn |
| POST | `/api/v1/link/install-applet` | Install bundled NeoLinkChat.OS3KApp |

## Hardware tips

1. Flash with `CONFIG_BUDDY_NEO_LINK` (default in `sdkconfig.defaults`).
2. **Install the applet from the buddy** (bundled `.OS3KApp`, ~3.5 KB, v0.8+):
   - Portal **Neo Link** → **Install bundled applet**, or
   - UART: `login …` then `link install`
3. Open **Neo Link Chat** (Left Shift+Tab at power-on).
4. Test buddy→Neo alone: `POST /link/send` then press **Find** in the applet.
5. Test Neo→buddy without HID: type prompt, Send (writes Out), then `POST /link/pull`.
6. Test full HID path when QueueKey reaches `/api/keyboard/recent`.

## Licence

GPL-3.0 (same as parent). BetaWise is MIT — build against your own tree.
