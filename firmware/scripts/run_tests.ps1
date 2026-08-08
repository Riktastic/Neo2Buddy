# Run firmware unit tests (Unity) and production build.
# Requires ESP-IDF environment (see firmware/idf_env.ps1).

param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\idf_env.ps1"

Push-Location "$PSScriptRoot\.."

Write-Host "== Building production firmware =="
idf build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== Building Unity test firmware =="
idf -C test build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "To run tests on device:"
    Write-Host "  cd firmware; . .\idf_env.ps1; idf -C test -p COMx flash monitor"
    Write-Host "Then press Enter in the monitor to run individual tests, or let the menu run all."
}

Pop-Location
