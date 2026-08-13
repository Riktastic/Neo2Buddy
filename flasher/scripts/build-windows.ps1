param(
    [string]$Version = "1.0.0",
    [string]$ImagesDir = ""
)

# Build a Windows Neo2BuddySetup folder (+ zip) with PyInstaller.
# Requires Python + pip; firmware images in -ImagesDir or releases/<ver>.

$ErrorActionPreference = "Stop"
$flasher = Resolve-Path (Join-Path $PSScriptRoot "..")
$repo = Resolve-Path (Join-Path $flasher "..")

if (-not $ImagesDir) {
    $ImagesDir = Join-Path $repo "releases\$Version"
}
if (-not (Test-Path (Join-Path $ImagesDir "littlefs.bin"))) {
    throw "Firmware images not found in $ImagesDir - build/package firmware first."
}

$stage = Join-Path $flasher "_native_stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path (Join-Path $stage "images") | Out-Null

Copy-Item (Join-Path $flasher "neo2buddy_flasher") (Join-Path $stage "neo2buddy_flasher") -Recurse
Copy-Item (Join-Path $flasher "run_setup.py") (Join-Path $stage "run_setup.py")
Copy-Item (Join-Path $flasher "neo2buddy_setup.spec") (Join-Path $stage "neo2buddy_setup.spec")
foreach ($bin in @(
    "bootloader.bin",
    "partition-table.bin",
    "alpha_smart_neo2_buddy.bin",
    "littlefs.bin"
)) {
    Copy-Item (Join-Path $ImagesDir $bin) (Join-Path $stage "images\$bin") -Force
}

Push-Location $stage
try {
    # Prefer a system Python with working Tk. IDF's venv often has broken tkinter.
    $localPy = Join-Path $env:LOCALAPPDATA "Python\bin\python.exe"
    $pyCandidates = @(
        $env:NEO2BUDDY_PYTHON,
        $localPy,
        (Get-Command py -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
        (Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    ) | Where-Object { $_ -and (Test-Path $_) }
    $python = $null
    foreach ($cand in $pyCandidates) {
        & $cand -c "import tkinter" 2>$null
        if ($LASTEXITCODE -eq 0) {
            $python = $cand
            break
        }
    }
    if (-not $python) {
        throw "No Python with working tkinter found. Set NEO2BUDDY_PYTHON to a Tk-capable interpreter."
    }
    Write-Host "Using Python: $python"
    & $python -m pip install -q "pyinstaller>=6.3" -r (Join-Path $flasher "requirements.txt")
    & $python -m PyInstaller --noconfirm neo2buddy_setup.spec
    $dist = Join-Path $stage "dist\Neo2BuddySetup"
    if (-not (Test-Path $dist)) { throw "PyInstaller did not produce $dist" }

    $outDir = Join-Path $repo "releases\$Version"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $zipPath = Join-Path $outDir "Setup-windows-$Version.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

    @(
        "Neo2 Buddy Setup $Version (Windows)"
        ""
        "1. Unzip this archive."
        "2. Run Neo2BuddySetup\Neo2BuddySetup.exe"
        "3. Plug in the board programming USB port, pick COM, Install firmware."
        ""
        "No Python install required."
        "Project: https://github.com/Riktastic/Neo2Buddy"
    ) | Set-Content -Path (Join-Path $dist "README.txt") -Encoding UTF8

    Compress-Archive -Path $dist -DestinationPath $zipPath -Force
    Write-Host "Built $zipPath"
} finally {
    Pop-Location
}
