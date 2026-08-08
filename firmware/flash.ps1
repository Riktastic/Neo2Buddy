# Flash Neo2 Buddy firmware to the ESP32-S3-DevKit-Lipo.
# Usage:  .\flash.ps1 COM3
#         .\flash.ps1 COM3 -Monitor
#         .\flash.ps1 COM3 -ManualBoot          # Olimex: hold BUT1, tap RST1, release BUT1
#         .\flash.ps1 COM3 -ManualBoot -Monitor

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Port,

    [switch]$Monitor,
    [switch]$BuildOnly,
    [switch]$ManualBoot,
    [switch]$Slow
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\idf_env.ps1"
Set-Location $PSScriptRoot

if (-not (Test-Path $script:IdfPy)) {
    Write-Error "idf.py not found at $script:IdfPy - is ESP-IDF installed at C:\Espressif?"
}

function Invoke-ManualEspFlash {
    param(
        [string]$Port,
        [string]$Baud
    )

    $buildDir = Join-Path $PSScriptRoot "build"
    $flashArgsPath = Join-Path $buildDir "flash_args"
    if (-not (Test-Path $flashArgsPath)) {
        Write-Error "Missing $flashArgsPath - build the firmware first."
    }

    $esptool = Join-Path $env:IDF_PATH "components/esptool_py/esptool/esptool.py"
    $lines = Get-Content $flashArgsPath | Where-Object { $_.Trim() -ne "" }
    $modeArgs = ($lines[0].Trim() -split '\s+')
    $imageArgs = @()
    for ($i = 1; $i -lt $lines.Count; $i++) {
        $parts = ($lines[$i].Trim() -split '\s+', 2)
        $imageArgs += $parts[0], $parts[1]
    }

    Push-Location $buildDir
    try {
        & python $esptool --chip esp32s3 -p $Port -b $Baud --before no_reset --after no_reset write_flash @modeArgs @imageArgs
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}

Write-Host "Building firmware..."
idf build
if ($BuildOnly) { exit 0 }

$baud = if ($Slow) { "115200" } else { "460800" }

Write-Host ""
Write-Host "=== Flash to $Port (baud $baud) ==="
Write-Host "Olimex board: BUT1 = boot, RST1 = reset. Use the USB-UART port (CH340)."
Write-Host ""

if ($ManualBoot) {
    Write-Host "Manual download mode:"
    Write-Host "  1. Hold BUT1"
    Write-Host "  2. Tap RST1 while holding BUT1"
    Write-Host "  3. Release BUT1"
    Write-Host "  4. Press Enter here within a few seconds..."
    Read-Host
    Write-Host "Flashing (no auto-reset)..."
    Invoke-ManualEspFlash -Port $Port -Baud $baud
} else {
    Write-Host "Trying auto-reset flash. If you get boot mode 0x28, rerun with -ManualBoot:"
    Write-Host "  .\flash.ps1 $Port -ManualBoot -Monitor"
    Write-Host ""
    idf -p $Port -b $baud flash
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Monitor) {
    Write-Host ""
    Write-Host "Flash complete. Press RST1 once, then press Enter to open the serial monitor..."
    Read-Host
    Write-Host "Opening serial monitor (Ctrl+] to exit)..."
    & "$PSScriptRoot\monitor.ps1" $Port
} elseif (-not $ManualBoot) {
    Write-Host ('Done. Open http://192.168.4.1/ after joining the setup Wi-Fi, or check serial with: .\flash.ps1 {0} -Monitor' -f $Port)
} else {
    Write-Host "Flash complete. Press RST1, then run: .\flash.ps1 $Port -Monitor"
    Write-Host ('Or open http://192.168.4.1/ after joining the setup Wi-Fi (password: neo2buddy)' -f $Port)
}
