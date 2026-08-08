# Neo2 Buddy firmware 0.9.1-dev

Flash with ESP-IDF esptool (adjust COM port / use flash_args from this folder):

python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 alpha_smart_neo2_buddy.bin 0x420000 littlefs.bin

Portal user guide is served from the device at /user-guide.html after flashing.
