param(
    [string]$Version = "1.0.0",
    [switch]$SkipBuild
)

# Build curated end-user profiles (full / headless / uart-slim) and package them.
# Requires ESP-IDF (source firmware/idf_env.ps1 first on Windows).

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$firmware = Join-Path $repo "firmware"
$packProfile = Join-Path $PSScriptRoot "package-profile.ps1"
$packRelease = Join-Path $PSScriptRoot "package-release.ps1"

. (Join-Path $firmware "idf_env.ps1")

function Invoke-ProfileBuild([string]$ProfileId, [string]$FragmentRel) {
    $buildDir = Join-Path $firmware "build-custom\$ProfileId"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $defaults = "sdkconfig.defaults"
    if ($FragmentRel) {
        $defaults = "sdkconfig.defaults;$FragmentRel"
    }
    Push-Location $firmware
    try {
        $sdk = Join-Path $buildDir "sdkconfig"
        if (Test-Path $sdk) { Remove-Item $sdk -Force }
        # Redirect IDF stdout so PowerShell does not capture it as the return value.
        & "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" $script:IdfPy `
            -B $buildDir `
            -D "SDKCONFIG=$buildDir/sdkconfig" `
            -D "SDKCONFIG_DEFAULTS=$defaults" `
            build *>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed for profile $ProfileId"
        }
    } finally {
        Pop-Location
    }
    return $buildDir
}

if (-not $SkipBuild) {
    Write-Host "=== Building full (default tree) ==="
    Push-Location $firmware
    try {
        & "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" $script:IdfPy build
        if ($LASTEXITCODE -ne 0) { throw "Full build failed" }
    } finally {
        Pop-Location
    }
}

& $packRelease -Version $Version

if (-not $SkipBuild) {
    Write-Host "=== Building headless ==="
    $null = Invoke-ProfileBuild "headless" "sdkconfig.d/profile_headless.defaults"
    & $packProfile -Version $Version -Profile headless -BuildDir (Join-Path $firmware "build-custom\headless")

    Write-Host "=== Building uart-slim ==="
    $null = Invoke-ProfileBuild "uart-slim" "sdkconfig.d/profile_uart_slim.defaults"
    & $packProfile -Version $Version -Profile uart-slim -BuildDir (Join-Path $firmware "build-custom\uart-slim")
} else {
    Write-Host "SkipBuild set - packaging from existing build-custom folders if present."
    foreach ($p in @("headless", "uart-slim")) {
        $bd = Join-Path $firmware "build-custom\$p"
        if (Test-Path (Join-Path $bd "alpha_smart_neo2_buddy.bin")) {
            & $packProfile -Version $Version -Profile $p -BuildDir $bd
        } else {
            Write-Warning "No build for $p at $bd"
        }
    }
}

Write-Host "Done. Setup discovers:"
Write-Host "  releases/$Version"
Write-Host "  releases/$Version-headless"
Write-Host "  releases/$Version-uart-slim"
