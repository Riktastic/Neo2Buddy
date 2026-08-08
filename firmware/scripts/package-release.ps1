param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$build = Join-Path $repo "firmware\build"
if (-not $Version) {
    $Version = Get-Date -Format "yyyyMMdd-HHmm"
}

$out = Join-Path $repo "releases\$Version"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$files = @(
    "bootloader\bootloader.bin",
    "partition_table\partition-table.bin",
    "ota_data_initial.bin",
    "alpha_smart_neo2_buddy.bin",
    "littlefs.bin",
    "flash_args"
)

foreach ($rel in $files) {
    $src = Join-Path $build $rel
    if (-not (Test-Path $src)) {
        Write-Warning "Missing $src"
        continue
    }
    $destName = Split-Path $rel -Leaf
    Copy-Item $src (Join-Path $out $destName) -Force
}

$readme = @"
# Neo2 Buddy firmware $Version

Flash with ESP-IDF esptool (adjust COM port / use flash_args from this folder):

python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 alpha_smart_neo2_buddy.bin 0x420000 littlefs.bin

Portal user guide: /user-guide.html after flashing.

## Known behaviour

- Neo USB-B needs 5 V for enumeration (see docs/neo2-usb-wiring.md).
- Backup / scan / read / write interrupt Neo keyboard mode. Backup now/all return to keyboard; a single Read may need Keyboard mode.
- BLE pairs the buddy as a keyboard for portal Send text only — Neo keys are not forwarded over Bluetooth.
"@
Set-Content -Path (Join-Path $out "README.md") -Value $readme -Encoding UTF8

Write-Host "Release packaged at $out"
Get-ChildItem $out | Format-Table Name, Length
