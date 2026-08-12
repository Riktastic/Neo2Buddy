# Neo2 Buddy Setup & Image Builder

Two desktop tools live in `flasher/`:

| App | Who | Needs ESP-IDF? |
|-----|-----|----------------|
| **Setup** (`neo2buddy_flasher`) | Everyone | No — flashes prebuilt image packs |
| **Image Builder** (`neo2buddy_flasher.builder_app`) | Developers / power users | Yes — toggles features and runs `idf.py` |

Visual language matches the portal (paper / ink / coral).

## Downloads (GitHub Releases)

| Asset | What it is |
|-------|------------|
| `neo2buddy-setup-windows-*.zip` | **`.exe`** Setup app (no Python) |
| `neo2buddy-setup-macos-*.zip` | **`.app`** |
| `neo2buddy-setup-linux-*.zip` | Native binary folder |
| `neo2buddy-setup-*.zip` | Python source + Full images (`Run Setup.bat` / `run-setup.sh`) |
| `neo2buddy-firmware-*.zip` | Raw Full bins |
| `neo2buddy-firmware-*-headless.zip` | Optional lean pack (no OLED/SD) |
| `neo2buddy-firmware-*-uart-slim.zip` | Optional UART-only pack |

Native Setup builds are produced by GitHub Actions (PyInstaller) on release tags. Lean firmware zips are packaged locally with `firmware/scripts/package-profiles.ps1`.

## Setup (end users)

| | |
|--|--|
| **Folder** | `flasher/` |
| **Run** | `python -m neo2buddy_flasher` |
| **Shortcut** | `neo2buddy-flash` (after `pip install -e .`) |

```powershell
cd flasher
python -m venv .venv
.\.venv\Scripts\Activate.ps1   # macOS/Linux: source .venv/bin/activate
pip install -r requirements.txt
python -m neo2buddy_flasher
```

Use a normal Python install (with Tk). On some Linux distros: `python3-tk`.

1. Plug in the Buddy’s **programming USB** cable (not the Neo keyboard cable).
2. Click **Find ports** and pick the USB port.
3. Choose a **Firmware profile** (Full by default).
4. Click **Install firmware**.
5. Follow the on-screen next steps (Wi‑Fi portal, or UART help for slim profiles).

If install fails: hold **BOOT**, tap **RESET**, release **BOOT**, then Install again (Advanced → board already in download mode).

### Firmware profiles

Setup discovers complete image folders under `releases/`:

| Profile | Folder | Contents |
|---------|--------|----------|
| Full | `releases/1.0.0/` | Stock portal + BLE + Applet Store + OLED + SD |
| Headless | `releases/1.0.0-headless/` | No OLED / microSD |
| UART slim | `releases/1.0.0-uart-slim/` | Serial + Neo USB only |

Custom Image Builder exports also appear under `releases/custom/<name>/`.

Each folder needs:

- `bootloader.bin`
- `partition-table.bin`
- `alpha_smart_neo2_buddy.bin`
- `littlefs.bin`

### Build a Windows Setup .exe locally

```powershell
# After packaging firmware into releases/1.0.0/
.\flasher\scripts\build-windows.ps1 -Version 1.0.0
```

## Image Builder (developers — ESP-IDF required)

Build lean or custom firmware with a GUI that matches Setup:

```powershell
cd flasher
.\.venv\Scripts\Activate.ps1
pip install -e .
python -m neo2buddy_flasher.builder_app
# or: neo2buddy-build
# or: python run_builder.py
```

Source `firmware/idf_env.ps1` (or set `IDF_PATH` / `IDF_PYTHON_ENV_PATH`) first.

Presets map to `firmware/sdkconfig.d/profile_*.defaults`. Builds use an isolated
`SDKCONFIG` under `firmware/build-custom/<name>/` so the main Full tree is not
overwritten. Exports land in `releases/custom/<name>/` and can be flashed from
the builder or picked in Setup.

| Preset | Typical use |
|--------|-------------|
| Full | Same as stock release |
| Headless | Board without OLED / SD |
| UART slim | No Wi‑Fi portal — serial + Neo USB |
| No Bluetooth | Portal without BLE HID |

To package curated release profiles after building:

```powershell
.\firmware\scripts\package-profiles.ps1 -Version 1.0.0
```

## Tips

- Default install speed is fine for most boards; try **115200** under Advanced if installs fail.
- Full profile: join Buddy Wi‑Fi and open the portal (fresh SoftAP: `http://192.168.4.1/`).
- UART slim: serial console at **115200** baud — no SoftAP/portal.
- Close any serial monitor (miniterm, Putty, IDF monitor) before flashing — the COM port must be free.
