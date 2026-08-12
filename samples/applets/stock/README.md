# Stock SmartApplets

Eleven optional apps bundled in Neo2 Buddy firmware and offered in the portal **Applet Store** (Full / Wi‑Fi builds with `SUPPORT_STOCK_APPLETS`).

Rebuild (Docker Desktop required):

```powershell
.\tools\build-stock-applets.ps1
```

Outputs land in this folder and `firmware/main/neo/embedded/`.

| Applet | ID | Keys / idea |
|--------|----|-------------|
| **Dice Table** | `0xA1B2` | Notes left, dice right. **F1–F7** = d4…d100. **Space** = 2d6. **F8** undo. Nat 20 / nat 1 flash. |
| **Snake** | `0xA1B8` | Eat `*`, grow, speed up. Arrows steer (same again = nudge). **Space** pause. Best saved. |
| **Hang Word** | `0xA1B9` | Guess letters (6 lives). **Tab** = hint (costs a life). Streaks tracked. |
| **Tic Tac Toe** | `0xA1BA` | You=X vs Neo. **Tab** Easy/Hard. Arrows+Enter or keys **1–9**. |
| **Task Pad** | `0xA1B1` | Checklist. Enter add, Space toggle, Delete remove, Find save. |
| **Script Pad** | `0xA1B3` | Screenplay lines. Tab inserts next speaker cue. Enter=line. |
| **Word Tree** | `0xA1B4` | Counts **AlphaWord** words on-device. Enter=goal, Find=refresh. |
| **Type Drill** | `0xA1B5` | Timed prompt → WPM. **Find** peeks next prompt. Esc cancels. |
| **Touch Type** | `0xA1BB` | Learn touch typing. 5 lessons, finger hints, live WPM. Wrong key = stay put. |
| **Flash Cards** | `0xA1B6` | Multi-set editor in portal; push one deck to Neo. **Tab** = Show / Reverse / Type / Type↔. Show: Space+Y/N. Type: Enter check, Find=hint. Clear File=EN→NL starter. |
| **Math Drill** | `0xA1B7` | Arith / algebra / units. Enter=answer, Tab=category, Find=skip. Streaks. |

### Flash Cards decks

Buddy stores named sets (max 16 cards × 23 chars). Seed set: **English to Dutch**. Portal **Deck library** edits sets and **Push to Neo**. Import Anki Notes TXT / CSV / `front|back`. The Neo applet holds one active deck.

Open on the Neo via **Applets** after installing from the buddy portal.
