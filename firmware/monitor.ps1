# Open serial monitor for the Olimex ESP32-S3-DevKit-Lipo (CH340 on COM port).
# Usage:  .\monitor.ps1 COM3
#         .\monitor.ps1 COM3 -IdfMonitor   # use idf monitor (gdb decode, filters)

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Port,

    [int]$Baud = 115200,
    [switch]$IdfMonitor
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\idf_env.ps1"
Set-Location $PSScriptRoot

Write-Host "Serial monitor on $Port at $Baud baud"
Write-Host "Press RST1 on the board if you see no output."
Write-Host "Quit: Ctrl+] (miniterm) or Ctrl+] (idf monitor)"
Write-Host ""

if ($IdfMonitor) {
    # Port must be on the monitor subcommand (not global -p), or idf falls back to esptool auto-detect.
    idf monitor --port $Port --no-reset -b $Baud
} else {
    # Plain UART reader - no esptool, no DTR/RTS reset (best for Olimex CH340).
    & python -m serial.tools.miniterm $Port $Baud
}
