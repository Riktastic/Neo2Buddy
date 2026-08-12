# Neo2 Buddy firmware 1.0.0-uart-slim

UART slim: serial console + Neo USB only (no Wi-Fi/web, BLE, App Store, OLED, SD).

## Setup utility

Use **Neo2 Buddy Setup** and pick this folder as the firmware profile
(`releases/1.0.0-uart-slim`), or Advanced â†’ Choose folder.

UART slim: after install, open a serial terminal at 115200 baud (no Wiâ€‘Fi portal).

## esptool

```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 alpha_smart_neo2_buddy.bin 0x1C0000 littlefs.bin
```
