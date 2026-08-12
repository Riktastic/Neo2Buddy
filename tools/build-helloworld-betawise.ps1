# Build BetaWise v0.2 HelloWorld.OS3KApp via Docker (wischner/gcc-m68k).
# Output: samples/applets/HelloWorld.OS3KApp
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildRoot = Join-Path $RepoRoot "tools\betawise-v0.2-build"
$OutDir = Join-Path $RepoRoot "samples\applets"
$Tag = "v0.2"

function Write-UnixText {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Content)
    $normalized = $Content -replace "`r`n", "`n" -replace "`r", "`n"
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, $normalized, $utf8NoBom)
}

if (-not (Test-Path (Join-Path $BuildRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot "tools") | Out-Null
    if (Test-Path $BuildRoot) {
        Remove-Item -Recurse -Force $BuildRoot
    }
    git clone --depth 1 --branch $Tag https://github.com/isotherm/betawise.git $BuildRoot
} else {
    Push-Location $BuildRoot
    try {
        git fetch --tags --force
        git checkout -f $Tag
    } finally {
        Pop-Location
    }
}

Write-UnixText (Join-Path $BuildRoot "versiongen.py") @'
#!/bin/sh
cat <<EOF
#define BETAWISE_VERSION_MAJOR 0
#define BETAWISE_VERSION_MINOR 2
#define BETAWISE_VERSION_REVISION " "
EOF
'@

Write-UnixText (Join-Path $BuildRoot "version.h") @'
#define BETAWISE_VERSION_MAJOR 0
#define BETAWISE_VERSION_MINOR 2
#define BETAWISE_VERSION_REVISION " "
'@

# BusyBox find lacks -printf; build libos3k only (do not recurse into applets).
$makefile = Join-Path $BuildRoot "Makefile"
Write-UnixText $makefile @'
INT = version.h
OBJ = os3k.o syscall.o
TARGET = libos3k.a

include Makefile.common

os3k.o: version.h

version.h: versiongen.py
	./versiongen.py > version.h
'@

Write-UnixText (Join-Path $BuildRoot "docker-build-helloworld.sh") @'
#!/bin/sh
set -e
cd /src
sed -i "s/\r$//" Makefile Makefile.common HelloWorld/Makefile versiongen.py 2>/dev/null || true
chmod +x versiongen.py
./versiongen.py > version.h
make clean || true
make all
make -C HelloWorld clean all
ls -la HelloWorld/HelloWorld.OS3KApp libos3k.a
'@

if ($BuildRoot -match '^([A-Za-z]):\\(.*)$') {
    $mount = "$($Matches[1].ToLower()):/$($Matches[2] -replace '\\', '/')"
} else {
    throw "Unexpected build path: $BuildRoot"
}

docker pull wischner/gcc-m68k:latest | Out-Host
docker run --rm -v "${mount}:/src" -w /src wischner/gcc-m68k:latest sh /src/docker-build-helloworld.sh

$built = Join-Path $BuildRoot "HelloWorld\HelloWorld.OS3KApp"
if (-not (Test-Path $built)) {
    throw "Docker build did not produce HelloWorld.OS3KApp"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $built (Join-Path $OutDir "HelloWorld.OS3KApp") -Force
Copy-Item (Join-Path $BuildRoot "HelloWorld\HelloWorld.c") (Join-Path $OutDir "HelloWorld.c") -Force

$embedDir = Join-Path $RepoRoot "firmware\main\neo\embedded"
New-Item -ItemType Directory -Force -Path $embedDir | Out-Null
Copy-Item $built (Join-Path $embedDir "HelloWorld.OS3KApp") -Force

$out = Join-Path $OutDir "HelloWorld.OS3KApp"
$bytes = [IO.File]::ReadAllBytes($out)
if ($bytes.Length -lt 132) { throw "OS3KApp too small" }
$sig = ([uint32]$bytes[0] -shl 24) -bor ([uint32]$bytes[1] -shl 16) -bor ([uint32]$bytes[2] -shl 8) -bor [uint32]$bytes[3]
$footerOff = $bytes.Length - 4
$footer = ([uint32]$bytes[$footerOff] -shl 24) -bor ([uint32]$bytes[$footerOff + 1] -shl 16) -bor ([uint32]$bytes[$footerOff + 2] -shl 8) -bor [uint32]$bytes[$footerOff + 3]
if ($sig -ne 0xC0FFEEAD -or $footer -ne 0xCAFEFEED) {
    throw "Invalid applet signatures"
}

$sha = (Get-FileHash -Path $out -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "OK: $out ($($bytes.Length) bytes)"
Write-Host "BetaWise: $Tag (Hello World id=0xA1A0, version 0.2)"
Write-Host "SHA256: $sha"
