# Neo2 Buddy firmware 1.0.0-beta.1

Flash with ESP-IDF esptool (adjust COM port / use flash_args from this folder):

python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 alpha_smart_neo2_buddy.bin 0x420000 littlefs.bin

Portal user guide: /user-guide.html after flashing.

## Known behaviour

- Neo USB-B needs 5 V for enumeration (see docs/neo2-usb-wiring.md).
- Backup / scan / read / write interrupt Neo keyboard mode. Backup now/all return to keyboard; a single Read may need Keyboard mode.
- BLE pairs the buddy as a keyboard for portal Send text only â€” Neo keys are not forwarded over Bluetooth.
