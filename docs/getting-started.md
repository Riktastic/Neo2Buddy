# Getting started

You do not need to be a programmer. You need a Neo2, a small ESP32 board, a way to give the Neo **5 V on its USB-B port**, and about an evening.

## What you need

| Item | Why |
|------|-----|
| **AlphaSmart Neo2** | The writing machine. Uses the square **USB-B** port on the back. |
| **ESP32-S3 board with a real USB OTG / host port** | This *is* the Buddy. It must have a USB port for talking to the Neo **and** a separate port for installing firmware. About **€6–€15**. **8 MB flash** is recommended. A well-tested board is the [Olimex ESP32-S3-DevKit-Lipo](https://www.olimex.com/Products/IoT/ESP32-S3/) (OTG1 = Neo, other USB = flashing). |
| **5 V for the Neo USB port** | The Neo will not talk USB on AA batteries alone. Use a **small USB power bank**, or — if you already did the [USB lamp mod](cable.md#the-usb-lamp-mod) — that same idea. |
| **A custom USB cable** | Three ends: USB-B to the Neo, USB-A to the power bank, USB-C or Micro-USB to the Buddy’s **OTG** port. [How to make it](cable.md). Thrift-store cables are fine. |
| **A phone or laptop** | For the web page (portal) over Wi‑Fi. |

Optional: a microSD card on the Buddy ([wiring](sd-card.md)) for lots of backups, and a tiny OLED if your board has one (shows the web address).

You do **not** need extra software. Download **Setup** from the [release](https://github.com/Riktastic/Neo2Buddy/releases) (`Setup-windows`, `Setup-macos`, or `Setup-linux`), unzip, and run it. It shows how to put the ESP32 into download mode and flashes the board. ESP-IDF is only for people who [build firmware themselves](flashing.md#other-ways-only-if-you-need-them).

## The idea in one picture

```text
Power bank  ── 5 V ──────────────►  Neo USB-B
ESP32 OTG   ── data (D+ / D−) ──►  Neo USB-B
Everything  ── ground together
```

Write on the Neo as usual. Plug the Buddy in when you want backups or Bluetooth. Unplug when you don’t.

## Do this in order

1. **Make or check the cable** — [Cable & power](cable.md).
2. **Put firmware on the ESP32** — [Flashing](flashing.md). Use **Setup** from the release. Use the **programming / UART** USB port, not the Neo cable.
3. **First time in the portal** (Wi‑Fi builds — the usual “Full” image):
   1. Power the Buddy.
   2. On your phone or laptop, join the Wi‑Fi hotspot whose name starts with **Neo2**.
   3. Open [http://192.168.4.1](http://192.168.4.1) (or the address on the OLED).
   4. Choose **Direct access** (keep the hotspot) or **Home network** (join your router), and set a **portal password** (8+ characters).
   5. After it reboots, sign in with that password.
   6. Connect the power bank and the Neo cable, then tap **Backup now**.

Until you finish that setup, a fresh board uses the password `neo2buddy`.

On home Wi‑Fi, open the **IP address** the Buddy shows (OLED, if fitted) or prints on the serial log after it joins the router — something like `http://192.168.1.42`. There is no `http://….local` name; mDNS was removed.

UART-only images have no web page. Use a serial terminal at **115200** baud instead — see [Flashing](flashing.md).

## What “worth it” looks like day to day

- Finish a writing session, plug in, walk away: changed files are saved, Neo goes back to typing.
- Sit with the Neo in your lap and type into Google Docs / Word over Bluetooth.
- Glance at a phone to see the last lines you typed.
- Keep a copy in [Hammer](https://hammer.ink/) (recommended), or on Nextcloud / a NAS, if you want a second home for drafts.

Next: [Using Neo2 Buddy](using.md). Stuck? [Troubleshooting](troubleshooting.md).
