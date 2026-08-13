param(
    [string]$Version = "1.0.0",
    [string]$Profile = "full",
    [string]$BuildDir = ""
)

# Package firmware images from a build directory into releases/<Version>[-profile].
# Profile: full | headless | uart-slim | no-ble

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")

if (-not $Version) { $Version = "1.0.0" }
$Profile = $Profile.ToLowerInvariant()

$suffix = ""
if ($Profile -ne "full") {
    $suffix = "-$Profile"
}

if (-not $BuildDir) {
    if ($Profile -eq "full") {
        $BuildDir = Join-Path $repo "firmware\build"
    } else {
        $BuildDir = Join-Path $repo "firmware\build-custom\$Profile"
    }
}

$out = Join-Path $repo "releases\$Version$suffix"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$files = @(
    "bootloader\bootloader.bin",
    "partition_table\partition-table.bin",
    "alpha_smart_neo2_buddy.bin",
    "littlefs.bin",
    "flash_args"
)

foreach ($rel in $files) {
    $src = Join-Path $BuildDir $rel
    if (-not (Test-Path $src)) {
        Write-Warning "Missing $src"
        continue
    }
    $destName = Split-Path $rel -Leaf
    Copy-Item $src (Join-Path $out $destName) -Force
}

$profileNote = switch ($Profile) {
    "full" { "Full build (Wi-Fi portal, BLE, Applet Store, OLED, SD)." }
    "headless" { "Headless: no OLED / microSD. Portal + BLE + App Store remain." }
    "uart-slim" { "UART slim: serial console + Neo USB only (no Wi-Fi/web, BLE, App Store, OLED, SD)." }
    "no-ble" { "Full portal features without Bluetooth HID." }
    default { "Custom profile: $Profile" }
}

$readme = @"
# Neo2 Buddy firmware $Version$suffix

$profileNote

## Setup (recommended)

Download **Setup** from the GitHub release (``Setup-windows``, ``Setup-macos``, or ``Setup-linux``), unzip, and run it. Pick this folder as the firmware profile
(``releases/$Version$suffix``), or Advanced → Choose folder.

UART slim: after install, open a serial terminal at 115200 baud (no Wi‑Fi portal).

How to flash: https://github.com/Riktastic/Neo2Buddy/blob/main/docs/flashing.md

## esptool

``````
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 alpha_smart_neo2_buddy.bin 0x1C0000 littlefs.bin
``````
"@
Set-Content -Path (Join-Path $out "README.md") -Value $readme -Encoding UTF8

$profileTxt = @"
label=$Profile
version=$Version
profile=$Profile
"@
Set-Content -Path (Join-Path $out "profile.txt") -Value $profileTxt -Encoding UTF8

# Firmware-only zip for non-full profiles (full still uses package-release.ps1 for Setup zip).
if ($Profile -ne "full") {
    $fwZip = Join-Path $out "neo2buddy-firmware-$Version$suffix.zip"
    if (Test-Path $fwZip) { Remove-Item $fwZip -Force }
    $staging = Join-Path $out "_fw_zip_staging"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    foreach ($bin in @(
        "bootloader.bin",
        "partition-table.bin",
        "alpha_smart_neo2_buddy.bin",
        "littlefs.bin",
        "flash_args",
        "README.md",
        "profile.txt"
    )) {
        $p = Join-Path $out $bin
        if (Test-Path $p) { Copy-Item $p (Join-Path $staging $bin) -Force }
    }
    Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $fwZip -Force
    Remove-Item -Recurse -Force $staging
    Write-Host "Profile pack: $fwZip"
}

Write-Host "Packaged profile '$Profile' at $out"
Get-ChildItem $out | Format-Table Name, Length
