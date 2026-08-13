# Neo2 Buddy Setup and Image Builder

**Users:** download **Setup** from a [GitHub release](https://github.com/Riktastic/Neo2Buddy/releases) — `Setup-windows`, `Setup-macos`, or `Setup-linux`. Unzip and run it. No Python or ESP-IDF. How to use it: [docs/flashing.md](../docs/flashing.md).

This folder is the **source** of that app (and of Image Builder).

| App | Command | Needs ESP-IDF? |
|-----|---------|----------------|
| **Setup** | `python -m neo2buddy_flasher` | No — flashes image packs |
| **Image Builder** | `python -m neo2buddy_flasher.builder_app` | Yes |

## Run Setup from this repo

```powershell
cd flasher
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python -m neo2buddy_flasher
```

Needs a Python with Tk. Plug the **programming** USB port, pick COM, pick a profile, Install.

Setup looks under `releases/` for complete folders (`bootloader.bin`, `partition-table.bin`, `alpha_smart_neo2_buddy.bin`, `littlefs.bin`), including `releases/custom/`.

## Image Builder from this repo

```powershell
cd firmware
. .\idf_env.ps1
cd ..\flasher
pip install -e .
python -m neo2buddy_flasher.builder_app
```

## Build the Windows Setup zip locally

```powershell
.\flasher\scripts\build-windows.ps1 -Version 1.0.0
```

Produces `releases/1.0.0/Setup-windows-1.0.0.zip`. macOS and Linux zips are built by [`.github/workflows/release.yml`](../.github/workflows/release.yml) on a version tag.
