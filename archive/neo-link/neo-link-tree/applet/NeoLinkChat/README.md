# NeoLinkChat (BetaWise applet)



Chat UI for Neo2 Buddy LLM proxy. Uses **NeoLinkOut file** (primary) plus **streamed HLT over HID** (optional).



**Read first:** [`../../docs/requirements.md`](../../docs/requirements.md) — what we must/must not do.  
[`../../docs/os3k-applet.md`](../../docs/os3k-applet.md) — OS3K coordinates, RAM, open path.



## Files on device



| Index | Logical name | Who writes | Purpose |

|-------|--------------|------------|---------|

| 1 | NeoLinkIn | Buddy (ASM) | LLM / echo reply |

| 2 | NeoLinkOut | Applet (`FileWriteBuffer`) | Prompt for `/api/v1/link/pull` |



Header: `fileCount=2`, `fileUsage=1024` (`NEO_LINK_APPLET_FILE_USAGE`), id `0xA1C0`.



Shared limits: [`../../protocol/neo_link_limits.h`](../../protocol/neo_link_limits.h).  
Guards: [`../neo_link_applet_guard.h`](../neo_link_applet_guard.h).  
Verify: [`../../../tools/verify_neo_link_applet.py`](../../../tools/verify_neo_link_applet.py) (runs at end of `build-docker.ps1`).

LCD layout: [`../neo_link_os3k.h`](../neo_link_os3k.h).



## Prebuilt binary



`NeoLinkChat.OS3KApp` in this folder is copied into `firmware/main/neo/embedded/` for buddy install.



**Install:**



1. Connect Neo to buddy.

2. Portal → **Neo Link** → **Install bundled applet** (replace), or UART `link install`.

3. Power Neo off → **Left Shift + Tab** → power on → **Neo Link Chat**.



## Rebuild (Docker)



```powershell

# from repo root

.\neo-link\applet\NeoLinkChat\build-docker.ps1

```



Build links only **`NeoLinkChat.o` + `neo_link_emit.o`** (not `neo_link_protocol.c`). Script fails if `ramUsage > 1536`.



## Usage



1. Enable `CONFIG_BUDDY_NEO_LINK`, configure LLM on portal.

On open you should see **v0.8c** on row 3. If not, reinstall the applet from the buddy.

3. **Enter** → type question → **Enter/Send**.

4. **Find** loads NeoLinkIn reply; **Up/Down** scrolls.



**Path 2 (reliable):** Send on Neo → portal **Fetch prompt from Neo & reply** → **Find** on Neo.



## Keys



| Key | Action |

|-----|--------|

| Enter / Send | Compose (then send) |

| Find | Reload NeoLinkIn |

| Up / Down | Scroll reply |

| Home / End | Top / bottom |

| Esc | Cancel compose |

