# Neo Link hardware validation

Prove these paths **in order** before polishing further. Use the portal **Neo Link** page (or curl with a Bearer token).

Neo Link is a **separate SmartApplet** (`0xA1C0`). It does **not** use AlphaWord files.  
Requirements: [`requirements.md`](requirements.md).

## Prerequisites

1. Flash firmware with `CONFIG_BUDDY_NEO_LINK=y`
2. **Neo Link Chat v0.8c** is installed automatically when the Neo connects (`ramUsage` &lt; 800). Confirm row 3 shows **0.8c** on the Neo, or portal **Neo Link** shows `applet_up_to_date: true`.
3. Portal → **Neo Link** → configure LLM → **Test API**
4. Neo plugged into buddy USB

## Path 1 — Buddy → Neo (mailbox only)

1. Portal: **Send test reply → Neo**
2. On Neo: open Neo Link Chat → **Find**
3. Expect typing animation + text `Path1 OK…`

| Pass? | Notes |
|-------|-------|
| [ ] | |

## Path 2 — Neo → Buddy via file Outbox (no HID)

1. On Neo: **Enter** → type a short question → **Send** (writes `NeoLinkOut`)
2. Portal: **Fetch prompt from Neo & reply**
3. On Neo: wait or **Find**
4. Expect GPT (or echo) reply on screen; ↑/↓ if long

| Pass? | Notes |
|-------|-------|
| [ ] | |

## Path 3 — Full HID auto (optional)

1. Portal LLM enabled and working
2. On Neo: Send a prompt and **do not** press portal Pull
3. After ~8s countdown, applet auto-tries Find
4. Expect reply without portal button

| Pass? | Notes |
|-------|-------|
| [ ] | If fail, Path 2 is the supported fallback |

## Guards to confirm

- [ ] Status shows `alphaword_coupled: false`
- [ ] Rate limit: spam Test API → eventually `rate limit (N/min)`
- [ ] Follow-up question uses prior context (context_stored increases)
- [ ] Clear chat context resets follow-ups
- [ ] AlphaWord documents untouched after Neo Link use
