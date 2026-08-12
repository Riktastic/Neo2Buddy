param(
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$build = Join-Path $repo "firmware\build"
if (-not $Version) {
    $Version = "1.0.0"
}

$out = Join-Path $repo "releases\$Version"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$files = @(
    "bootloader\bootloader.bin",
    "partition_table\partition-table.bin",
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

## Easiest: Setup utility (no ESP-IDF)

Download **``neo2buddy-setup-$Version.zip``** from the [GitHub release](https://github.com/Riktastic/Neo2Buddy/releases) for this version.

1. Unzip it.
2. Double-click ``Run Setup.bat`` (Windows) or see README inside the zip for macOS/Linux.
3. Plug in the board’s **programming USB** port, pick the COM port, click **Install firmware**.

Requires [Python 3.10+](https://www.python.org/downloads/) with Tk (included on Windows/macOS).

## Advanced: esptool

Flash with ESP-IDF / esptool (adjust COM port):

``````
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 alpha_smart_neo2_buddy.bin 0x1C0000 littlefs.bin
``````

Or use ``flash_args`` from this folder.

Portal user guide: ``/user-guide.html`` after flashing.
Python client: ``python-wrapper/`` (``neo2buddy`` CLI).

## Known behaviour

- Neo USB-B needs 5 V for enumeration (see docs/neo2-usb-wiring.md).
- Backup / scan / read / write interrupt Neo keyboard mode. Backup now/all return to keyboard; a single Read may need Keyboard mode.
- BLE: use **Pair keyboard** in the portal (2-minute window). Neo keys pass through while connected; Documents/ASM actions can pause keystrokes until keyboard mode returns.
- Applet Store installs are **paused** in 1.0.0 while stock SmartApplets receive more testing (binaries remain in firmware; Flash Cards deck library still works). Set ``STOCK_STORE_INSTALLS_ENABLED`` to 1 to re-enable.
- Optional lean profiles (build with ``firmware/scripts/package-profiles.ps1``): ``$Version-headless``, ``$Version-uart-slim``. Pick them in Setup → Firmware profile.
- Primary testing: Dutch-market Neo2. Other regional layouts may differ for live typing / Bluetooth / import-export.
"@
[System.IO.File]::WriteAllText(
    (Join-Path $out "README.md"),
    ($readme -replace "`r`n", "`n") + "`n",
    [System.Text.UTF8Encoding]::new($false)
)

# --- Setup utility zip for GitHub Releases (flasher + firmware images) ---
$setupRoot = Join-Path $out "neo2buddy-setup-$Version"
$setupImages = Join-Path $setupRoot "images"
$flasherSrc = Join-Path $repo "flasher"

if (Test-Path $setupRoot) {
    Remove-Item -Recurse -Force $setupRoot
}
New-Item -ItemType Directory -Force -Path $setupImages | Out-Null

Copy-Item (Join-Path $flasherSrc "neo2buddy_flasher") (Join-Path $setupRoot "neo2buddy_flasher") -Recurse -Force
Copy-Item (Join-Path $flasherSrc "requirements.txt") (Join-Path $setupRoot "requirements.txt") -Force
Copy-Item (Join-Path $flasherSrc "pyproject.toml") (Join-Path $setupRoot "pyproject.toml") -Force

foreach ($bin in @(
    "bootloader.bin",
    "partition-table.bin",
    "alpha_smart_neo2_buddy.bin",
    "littlefs.bin"
)) {
    $binSrc = Join-Path $out $bin
    if (Test-Path $binSrc) {
        Copy-Item $binSrc (Join-Path $setupImages $bin) -Force
    } else {
        Write-Warning "Setup zip missing firmware image: $bin"
    }
}

$setupReadme = @"
# Neo2 Buddy Setup $Version

Install firmware on your ESP32-S3 buddy board — **no ESP-IDF** needed.

## Windows

1. Install [Python 3.10+](https://www.python.org/downloads/) if needed (tick **Add python.exe to PATH**).
2. Double-click ``Run Setup.bat``.
3. Plug the board’s **programming / UART** USB cable into the PC.
4. Select the COM port → **Install firmware**.

## macOS / Linux

``````
cd neo2buddy-setup-$Version
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python -m neo2buddy_flasher
``````

## Tips

- Use the programming USB port (CH340 / UART), not the Neo OTG host port.
- If install fails: Advanced → **Hold buttons before install**, then hold BOOT, tap RESET, release BOOT, Install.
- Project page: https://github.com/Riktastic/Neo2Buddy
- After install, join the buddy Wi‑Fi and open the portal (fresh SoftAP: http://192.168.4.1/).
"@
Set-Content -Path (Join-Path $setupRoot "README.md") -Value $setupReadme -Encoding UTF8

$runBat = @"
@echo off
setlocal
cd /d "%~dp0"
echo Neo2 Buddy Setup $Version
echo.

where python >nul 2>&1
if errorlevel 1 (
  echo Python was not found. Install Python 3.10+ from https://www.python.org/downloads/
  echo Tick "Add python.exe to PATH", then run this again.
  pause
  exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
  echo Creating virtual environment...
  python -m venv .venv
  if errorlevel 1 (
    echo Failed to create .venv
    pause
    exit /b 1
  )
)

call ".venv\Scripts\activate.bat"
python -m pip install -q --upgrade pip
python -m pip install -q -r requirements.txt
if errorlevel 1 (
  echo Failed to install dependencies.
  pause
  exit /b 1
)

echo Starting Neo2 Buddy Setup...
python -m neo2buddy_flasher
set EXITCODE=%ERRORLEVEL%
if not "%EXITCODE%"=="0" pause
exit /b %EXITCODE%
"@
Set-Content -Path (Join-Path $setupRoot "Run Setup.bat") -Value $runBat -Encoding ASCII

$runShPath = Join-Path $setupRoot "run-setup.sh"
@(
    '#!/usr/bin/env bash'
    'set -euo pipefail'
    'cd "$(dirname "$0")"'
    'python3 -m venv .venv'
    '# shellcheck disable=SC1091'
    'source .venv/bin/activate'
    'python -m pip install -q --upgrade pip'
    'python -m pip install -q -r requirements.txt'
    'exec python -m neo2buddy_flasher'
) | Set-Content -Path $runShPath -Encoding UTF8

$zipPath = Join-Path $out "neo2buddy-setup-$Version.zip"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}
Compress-Archive -Path $setupRoot -DestinationPath $zipPath -Force
Remove-Item -Recurse -Force $setupRoot

# Also zip bare firmware bins for people who only want esptool assets
$fwZip = Join-Path $out "neo2buddy-firmware-$Version.zip"
if (Test-Path $fwZip) {
    Remove-Item $fwZip -Force
}
$fwZipStaging = Join-Path $out "_fw_zip_staging"
if (Test-Path $fwZipStaging) { Remove-Item -Recurse -Force $fwZipStaging }
New-Item -ItemType Directory -Force -Path $fwZipStaging | Out-Null
foreach ($bin in @(
    "bootloader.bin",
    "partition-table.bin",
    "alpha_smart_neo2_buddy.bin",
    "littlefs.bin",
    "flash_args",
    "README.md"
)) {
    $p = Join-Path $out $bin
    if (Test-Path $p) { Copy-Item $p (Join-Path $fwZipStaging $bin) -Force }
}
Compress-Archive -Path (Join-Path $fwZipStaging "*") -DestinationPath $fwZip -Force
Remove-Item -Recurse -Force $fwZipStaging

Write-Host "Release packaged at $out"
Write-Host "GitHub release assets:"
Write-Host "  - $zipPath"
Write-Host "  - $fwZip"
Get-ChildItem $out | Format-Table Name, Length
