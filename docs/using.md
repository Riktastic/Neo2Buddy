# Using Neo2 Buddy

This assumes firmware is on the board and you have completed [first-time portal setup](getting-started.md). The same ideas appear in the on-device [user guide](https://riktastic.github.io/Neo2Buddy/user-guide.html).

## The portal

Join the Buddy hotspot or your home Wi‑Fi, then open the address you used at setup ([http://192.168.4.1](http://192.168.4.1) on a fresh hotspot).

Sign in with the portal password you chose. **Documents** is backups and files. **Typing & Bluetooth** is live keys and pairing.

## Backups

Connect the [cable](cable.md) so the Neo has 5 V on USB-B and the Buddy is on the data wires.

- **Backup now** / **Auto on connect** — save AlphaWord files that changed, then return the Neo to typing.
- **Backup all** — save every non-empty file, then return to typing.
- Empty or whitespace-only slots are skipped.
- Prefer a microSD card when one is fitted; otherwise backups live on the Buddy’s own flash. Oldest copies on **internal** flash may be removed when space is tight. **Nothing on the SD card is deleted** by that cleanup or by factory reset.

Scan, Read, Write, and Backup switch the Neo into **manager mode**. Live typing (and Bluetooth keystrokes) pause until the job finishes. Backup now/all put typing back automatically. After a single Read you may need **Keyboard mode**.

## Live typing

While the Neo is in keyboard mode, the Typing page (and UART / OLED, if you have them) show what you type.

## Bluetooth

On **Typing & Bluetooth**:

1. **Pair keyboard** (a couple of minutes to add a *new* computer or phone).
2. On that device, connect to Neo2 Buddy as a keyboard. No PIN. If a six-digit code appears, confirm it.
3. Keys you press on the Neo are typed on that device. Bonds are remembered after reboot.

After a firmware update, remove Neo2 Buddy from the computer’s Bluetooth list and use **Forget bonded hosts** on the Buddy (Windows especially remembers the old keyboard).

**Send text** pastes a longer chunk from the portal to the same paired device. Manager-mode actions on Documents can pause Bluetooth typing until keyboard mode returns.

## Cloud copies (optional)

Under **Cloud sync**, point at WebDAV (Nextcloud, many NAS boxes), S3-compatible storage, or Hammer Ink. **Test connection** first. Local files stay on the Buddy; upload never deletes them.

Home Wi‑Fi with internet is required. S3 also needs a correct clock (the Buddy sets time over the network).

Optional: **Auto cloud upload after backup**. Provider details: [cloud sync](../firmware/docs/cloud-sync.md). To pull files from a PC on a schedule, see the [Python tool](../python-wrapper/README.md).

## Files and applets

From Documents you can read, download, write, or clear AlphaWord slots (after a scan).

You can install a `.os3kapp` file, download or remove an applet. **In 1.0.0 the Applet Store catalog does not install apps** while those applets are retested. Flash Cards **decks** can still be edited in the portal and pushed to the Neo.

## Password and reset

Change the password under Settings. Factory reset asks for the password again, clears settings and **internal** backups, and **keeps the SD card**. After a full flash or factory reset, setup password is `neo2buddy` until you finish first-run again.

## Serial console

If UART is enabled: `login` with your portal password, then `help`. UART-slim firmware uses this instead of the web page (115200 baud, programming USB port).
