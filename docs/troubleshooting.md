# Troubleshooting

## The Neo is not detected

1. USB-B has **power** (power bank, or your [lamp-mod](cable.md#the-usb-lamp-mod) path). AA cells alone are not enough for USB.
2. Data wires go to the ESP32 **OTG / host** port, not the flashing/UART port.
3. **Ground** is shared between power bank, ESP32, and Neo.
4. Firmware is actually on the board and the board is powered.
5. After a file operation, tap **Keyboard mode** if you only needed typing, not a backup.

More cable detail: [Cable & power](cable.md).

## Flashing fails or no COM port

- Use **Setup** from the [release](https://github.com/Riktastic/Neo2Buddy/releases) and follow its on-screen BOOT / RESET steps.
- Use the **programming** USB port, not OTG.
- Close other programs using that port.
- Hold **BOOT**, tap **RESET**, release **BOOT**, then Install / flash again.
- Try a slower speed (Setup → Advanced → 115200).
- Another cable or USB port on the PC.

## Cannot open the portal

- You flashed a **Wi‑Fi** image (Full or Headless), not UART slim.
- You joined the **Neo2…** hotspot (or the home network you chose).
- Address on hotspot: [http://192.168.4.1](http://192.168.4.1).
- After setup, use the **IP** on the OLED or in the serial boot log (e.g. `http://192.168.1.42`). There is no `.local` / mDNS name.
- If home Wi‑Fi failed, a recovery hotspot should appear so you can fix it.
- Default password before first setup / after factory reset: `neo2buddy`.

## Bluetooth is silent

- Pair from the portal (**Pair keyboard**), then connect on the phone/PC.
- Neo must be in **keyboard mode** (not in the middle of a backup).
- After flashing, forget the device on the PC and **Forget bonded hosts** on the Buddy.
- Confirm any 6-digit code; do not type a PIN.

## Backups look empty or “already saved”

Blank documents are skipped. **Backup now** skips a file whose text already exists on the Buddy. Use **Backup all** to force today’s copies.

## Applet Store will not install

In 1.0.0 store installs are **turned off** on purpose. You can still install a `.os3kapp` file you already have, and you can still edit Flash Cards decks.

## Still stuck

[Open an issue](https://github.com/Riktastic/Neo2Buddy/issues) with board type, firmware profile, and whether USB-B has 5 V.
