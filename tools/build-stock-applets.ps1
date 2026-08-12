# Build all stock SmartApplets (BetaWise v0.2) into samples/applets/stock/*.OS3KApp
# and copy into firmware/main/neo/embedded/ for the buddy App Store.
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildRoot = Join-Path $RepoRoot "tools\betawise-v0.2-build"
$SrcDir = Join-Path $RepoRoot "samples\applets\stock"
$OutDir = $SrcDir
$EmbedDir = Join-Path $RepoRoot "firmware\main\neo\embedded"
$Tag = "v0.2"
$Apps = @("DiceTable", "TaskPad", "ScriptPad", "WordTree", "TypingDrill", "FlashCards", "MathDrill", "Snake", "HangWord", "TicTacToe", "TouchType")

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

function Get-BeUInt32([byte[]]$buf, [int]$off) {
    return [uint32](
        ([uint64]$buf[$off] -shl 24) -bor
        ([uint64]$buf[$off + 1] -shl 16) -bor
        ([uint64]$buf[$off + 2] -shl 8) -bor
        [uint64]$buf[$off + 3]
    )
}

if (-not (Test-Path (Join-Path $BuildRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot "tools") | Out-Null
    if (Test-Path $BuildRoot) { Remove-Item -Recurse -Force $BuildRoot }
    git clone --depth 1 --branch $Tag https://github.com/isotherm/betawise.git $BuildRoot
} else {
    Push-Location $BuildRoot
    try { git fetch --tags --force; git checkout -f $Tag } finally { Pop-Location }
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
Write-UnixText (Join-Path $BuildRoot "Makefile") @'
INT = version.h
OBJ = os3k.o syscall.o
TARGET = libos3k.a
include Makefile.common
os3k.o: version.h
version.h: versiongen.py
	./versiongen.py > version.h
'@

$mc = Join-Path $BuildRoot "Makefile.common"
$mcText = [IO.File]::ReadAllText($mc)
if ($mcText -notmatch '/freestanding"') {
    $mcText = $mcText.Replace('-I"$(TOP)"', '-I"$(TOP)/freestanding" -I"$(TOP)"')
}
Write-UnixText $mc $mcText

$Free = Join-Path $BuildRoot "freestanding"
New-Item -ItemType Directory -Force -Path $Free | Out-Null
Write-UnixText (Join-Path $Free "stddef.h") @'
#ifndef _STDDEF_H
#define _STDDEF_H
typedef unsigned long size_t;
typedef long ptrdiff_t;
#define NULL ((void*)0)
#endif
'@
Write-UnixText (Join-Path $Free "stdint.h") @'
#ifndef _STDINT_H
#define _STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned int uintptr_t;
typedef int intptr_t;
#endif
'@
Write-UnixText (Join-Path $Free "stdbool.h") @'
#ifndef _STDBOOL_H
#define _STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#endif
'@
Write-UnixText (Join-Path $Free "string.h") @'
#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t num);
void *memset(void *ptr, int value, size_t num);
void *memmove(void *dst, const void *src, size_t num);
size_t strlen(const char *str);
char *strncpy(char *dst, const char *src, size_t num);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t num);
char *strstr(const char *haystack, const char *needle);
#endif
'@

# Name file syscalls (v0.2 ships them as SYS_A*)
$syscallPath = Join-Path $BuildRoot "syscall.c"
$syscall = [IO.File]::ReadAllText($syscallPath)
$syscall = $syscall -replace 'DEFINE_SYSCALL\(102, SYS_A198\);', 'DEFINE_SYSCALL(102, FileWriteBuffer);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(103, SYS_A19C\);', 'DEFINE_SYSCALL(103, FileReadBuffer);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(112, SYS_A1C0\); // new applet', 'DEFINE_SYSCALL(112, FileSetFolder); // applet file folder'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(112, SYS_A1C0\);', 'DEFINE_SYSCALL(112, FileSetFolder);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(114, SYS_A1C8\);', 'DEFINE_SYSCALL(114, FileOpen);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(115, SYS_A1CC\);', 'DEFINE_SYSCALL(115, FileClose);'
Write-UnixText $syscallPath $syscall

$buildSh = @'
#!/bin/sh
set -e
cd /src
sed -i "s/\r$//" Makefile Makefile.common versiongen.py syscall.c 2>/dev/null || true
chmod +x versiongen.py
./versiongen.py > version.h
make clean || true
make all
for app in DiceTable TaskPad ScriptPad WordTree TypingDrill FlashCards MathDrill Snake HangWord TicTacToe TouchType; do
  sed -i "s/\r$//" "$app/Makefile" 2>/dev/null || true
  make -C "$app" clean all
  ls -la "$app/$app.OS3KApp"
done
'@
Write-UnixText (Join-Path $BuildRoot "docker-build-stock.sh") $buildSh

foreach ($app in $Apps) {
    $dst = Join-Path $BuildRoot $app
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item (Join-Path $SrcDir "$app.c") (Join-Path $dst "$app.c") -Force
    Copy-Item (Join-Path $SrcDir "stock_math.h") (Join-Path $dst "stock_math.h") -Force
    Copy-Item (Join-Path $SrcDir "stock_fmt.h") (Join-Path $dst "stock_fmt.h") -Force
    Copy-Item (Join-Path $SrcDir "stock_os3k_files.h") (Join-Path $dst "stock_os3k_files.h") -Force
    Get-ChildItem $dst -File | ForEach-Object {
        Write-UnixText $_.FullName ([IO.File]::ReadAllText($_.FullName))
    }
    Write-UnixText (Join-Path $dst "Makefile") @"
OBJ = $app.o
TARGET = $app.OS3KApp
include ../Makefile.common
"@
}

if ($BuildRoot -match '^([A-Za-z]):\\(.*)$') {
    $mount = "$($Matches[1].ToLower()):/$($Matches[2] -replace '\\', '/')"
} else {
    throw "Unexpected build path: $BuildRoot"
}

docker pull wischner/gcc-m68k:latest | Out-Host
docker run --rm -v "${mount}:/src" -w /src wischner/gcc-m68k:latest sh /src/docker-build-stock.sh

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $EmbedDir | Out-Null
$expectSig = [Convert]::ToUInt32("C0FFEEAD", 16)
$expectFooter = [Convert]::ToUInt32("CAFEFEED", 16)

foreach ($app in $Apps) {
    $built = Join-Path $BuildRoot "$app\$app.OS3KApp"
    if (-not (Test-Path $built)) { throw "Missing $built" }
    $bytes = [IO.File]::ReadAllBytes($built)
    $sig = Get-BeUInt32 $bytes 0
    $footer = Get-BeUInt32 $bytes ($bytes.Length - 4)
    $ram = Get-BeUInt32 $bytes 8
    if ($sig -ne $expectSig -or $footer -ne $expectFooter) {
        throw "${app}: bad signatures"
    }
    if ($ram -gt 1600) { throw "${app}: ramUsage $ram too high" }
    Copy-Item $built (Join-Path $OutDir "$app.OS3KApp") -Force
    Copy-Item $built (Join-Path $EmbedDir "$app.OS3KApp") -Force
    $id = ([uint16]$bytes[20] -shl 8) -bor [uint16]$bytes[21]
    # fix id read with uint64
    $id = [uint16](([uint64]$bytes[20] -shl 8) -bor [uint64]$bytes[21])
    Write-Host ("OK {0}: {1} bytes ram={2} id=0x{3}" -f $app, $bytes.Length, $ram, $id.ToString("X4"))
}

Write-Host "Stock applets ready in $OutDir and $EmbedDir"
