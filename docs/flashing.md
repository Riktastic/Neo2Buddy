# Flashing the Buddy

**Use Setup.** That is the intended way to put firmware on the board. It is a small desktop app: it shows how to prepare the ESP32 (BOOT / RESET), picks the USB port, and installs the image. You do **not** install Python, ESP-IDF, or anything else.

Download from the **[GitHub release](https://github.com/Riktastic/Neo2Buddy/releases)**:

| File | Run |
|------|-----|
| `Setup-windows-….zip` | `Neo2BuddySetup.exe` |
| `Setup-macos-….zip` | `Neo2BuddySetup.app` (first time: right-click → Open) |
| `Setup-linux-….zip` | `./Neo2BuddySetup` |

GitHub also lists **Source code** for the tag. That is the repository snapshot — not the flasher.

Use the board’s **programming / UART** USB port (on Olimex: the CH340 port, **not** OTG1).

![Neo2 Buddy Setup](assets/setup-installer.png)

![Olimex in flashing mode](assets/olimex-esp32s3-flashing.jpg)

1. Unzip Setup and start it.
2. Plug in the programming USB cable.
3. Follow the on-screen steps (hold **BOOT**, tap **RESET**, release **BOOT** when it asks).
4. Choose a **firmware profile** (Full is the usual one) → **Install firmware**.

**Full** is the web portal + Bluetooth + optional OLED/SD. **Headless** is the same without OLED/SD. **UART slim** is serial console + Neo USB only (no Wi‑Fi page).

After Full/Headless: join the **Neo2…** Wi‑Fi hotspot and open [http://192.168.4.1](http://192.168.4.1). After UART slim: a serial terminal at **115200** baud.

Close any other serial program (Putty, IDF monitor) so the port is free.

---

## Other ways (only if you need them)

Most people can stop here. These two are for building firmware yourself.

### ESP-IDF (from the `firmware` folder)

Needs **[ESP-IDF v5.3.1](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/get-started/)**. On Windows the helper expects `C:\Espressif\frameworks\esp-idf-v5.3.1`.

```powershell
cd firmware
. .\idf_env.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Or `.\flash.ps1 COM3` (Windows). This builds the **Full** image. See [For developers](developers.md).

### Image Builder (custom mix of features)

Same ESP-IDF install. Toggles Wi‑Fi, Bluetooth, OLED, and so on, then builds. Images land in `releases/custom/<name>/` and can be flashed with **Setup**.

```powershell
cd firmware
. .\idf_env.ps1
cd ..\flasher
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -e .
python -m neo2buddy_flasher.builder_app
```

Details: [flasher/README.md](../flasher/README.md).

---

## After flashing

[First time in the portal](getting-started.md#do-this-in-order) · [Using Neo2 Buddy](using.md)
