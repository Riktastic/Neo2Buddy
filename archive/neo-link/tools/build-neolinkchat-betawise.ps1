# Build NeoLinkChat.OS3KApp with BetaWise v0.2 + NeoLinkIn/Out mailboxes.
# Requires Docker Desktop. Output: samples/applets/NeoLinkChat.OS3KApp
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildRoot = Join-Path $RepoRoot "tools\betawise-v0.2-build"
$SrcTree = Join-Path $RepoRoot "archive\neo-link\neo-link-tree"
$SrcApplet = Join-Path $SrcTree "applet\NeoLinkChat"
$AppletCommon = Join-Path $SrcTree "applet"
$Protocol = Join-Path $SrcTree "protocol"
$DstApplet = Join-Path $BuildRoot "NeoLinkChat"
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

# BusyBox find lacks -printf; build libos3k only.
Write-UnixText (Join-Path $BuildRoot "Makefile") @'
INT = version.h
OBJ = os3k.o syscall.o
TARGET = libos3k.a

include Makefile.common

os3k.o: version.h

version.h: versiongen.py
	./versiongen.py > version.h
'@

# Prefer freestanding includes (image has no newlib headers).
# Note: CFLAGS already contains -ffreestanding; match the include path specifically.
$mc = Join-Path $BuildRoot "Makefile.common"
$mcText = [System.IO.File]::ReadAllText($mc)
if ($mcText -notmatch '/freestanding"') {
    $mcText = $mcText.Replace('-I"$(TOP)"', '-I"$(TOP)/freestanding" -I"$(TOP)"')
    if ($mcText -notmatch '/freestanding"') {
        throw "Failed to patch Makefile.common for freestanding includes"
    }
}
Write-UnixText $mc $mcText

$Free = Join-Path $BuildRoot "freestanding"
New-Item -ItemType Directory -Force -Path $Free | Out-Null
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
#endif
'@
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

# v0.2 ships unnamed file syscalls; master names them. Same trap indices.
$syscallPath = Join-Path $BuildRoot "syscall.c"
$syscall = [System.IO.File]::ReadAllText($syscallPath)
$syscall = $syscall -replace 'DEFINE_SYSCALL\(102, SYS_A198\);', 'DEFINE_SYSCALL(102, FileWriteBuffer);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(103, SYS_A19C\);', 'DEFINE_SYSCALL(103, FileReadBuffer);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(114, SYS_A1C8\);', 'DEFINE_SYSCALL(114, FileOpen);'
$syscall = $syscall -replace 'DEFINE_SYSCALL\(115, SYS_A1CC\);', 'DEFINE_SYSCALL(115, FileClose);'
if ($syscall -notmatch 'DEFINE_SYSCALL\(102, FileWriteBuffer\)') {
    throw "Failed to patch FileWriteBuffer into syscall.c (BetaWise $Tag)"
}
if ($syscall -notmatch 'DEFINE_SYSCALL\(114, FileOpen\)') {
    throw "Failed to patch FileOpen into syscall.c (BetaWise $Tag)"
}
Write-UnixText $syscallPath $syscall

New-Item -ItemType Directory -Force -Path $DstApplet | Out-Null
Copy-Item (Join-Path $SrcApplet "NeoLinkChat.c") $DstApplet -Force
Copy-Item (Join-Path $SrcApplet "neo_link_emit.c") $DstApplet -Force
Copy-Item (Join-Path $SrcApplet "neo_link_inbox.h") $DstApplet -Force
Copy-Item (Join-Path $Protocol "neo_link_limits.h") $DstApplet -Force
Copy-Item (Join-Path $AppletCommon "neo_link_os3k.h") $DstApplet -Force
Copy-Item (Join-Path $AppletCommon "neo_link_applet_guard.h") $DstApplet -Force

Get-ChildItem $DstApplet -File | ForEach-Object {
    Write-UnixText -Path $_.FullName -Content ([System.IO.File]::ReadAllText($_.FullName))
}

Write-UnixText (Join-Path $DstApplet "Makefile") @'
OBJ = NeoLinkChat.o neo_link_emit.o
TARGET = NeoLinkChat.OS3KApp

include ../Makefile.common
'@

Write-UnixText (Join-Path $BuildRoot "docker-build-neolinkchat.sh") @'
#!/bin/sh
set -e
cd /src
sed -i "s/\r$//" Makefile Makefile.common NeoLinkChat/Makefile versiongen.py syscall.c 2>/dev/null || true
chmod +x versiongen.py
./versiongen.py > version.h
make clean || true
make all
make -C NeoLinkChat clean all
ls -la NeoLinkChat/NeoLinkChat.OS3KApp libos3k.a
'@

if ($BuildRoot -match '^([A-Za-z]):\\(.*)$') {
    $mount = "$($Matches[1].ToLower()):/$($Matches[2] -replace '\\', '/')"
} else {
    throw "Unexpected build path: $BuildRoot"
}

docker pull wischner/gcc-m68k:latest | Out-Host
docker run --rm -v "${mount}:/src" -w /src wischner/gcc-m68k:latest sh /src/docker-build-neolinkchat.sh

$built = Join-Path $DstApplet "NeoLinkChat.OS3KApp"
if (-not (Test-Path $built)) {
    throw "Docker build did not produce NeoLinkChat.OS3KApp"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $built (Join-Path $OutDir "NeoLinkChat.OS3KApp") -Force
Copy-Item $built (Join-Path $SrcApplet "NeoLinkChat.OS3KApp") -Force

$embedDir = Join-Path $RepoRoot "firmware\main\neo\embedded"
New-Item -ItemType Directory -Force -Path $embedDir | Out-Null
Copy-Item $built (Join-Path $embedDir "NeoLinkChat.OS3KApp") -Force

$out = Join-Path $OutDir "NeoLinkChat.OS3KApp"
$bytes = [IO.File]::ReadAllBytes($out)
if ($bytes.Length -lt 132) { throw "OS3KApp too small" }

function Get-BeUInt32([byte[]]$buf, [int]$off) {
    return [uint32](
        ([uint64]$buf[$off] -shl 24) -bor
        ([uint64]$buf[$off + 1] -shl 16) -bor
        ([uint64]$buf[$off + 2] -shl 8) -bor
        [uint64]$buf[$off + 3]
    )
}
function Get-BeUInt16([byte[]]$buf, [int]$off) {
    return [uint16](([uint64]$buf[$off] -shl 8) -bor [uint64]$buf[$off + 1])
}

$sig = Get-BeUInt32 $bytes 0
$footer = Get-BeUInt32 $bytes ($bytes.Length - 4)
$expectSig = [Convert]::ToUInt32("C0FFEEAD", 16)
$expectFooter = [Convert]::ToUInt32("CAFEFEED", 16)
if ($sig -ne $expectSig -or $footer -ne $expectFooter) {
    throw "Invalid applet signatures (sig=0x$($sig.ToString('X8')) footer=0x$($footer.ToString('X8')))"
}

$ram = Get-BeUInt32 $bytes 8
$rom = Get-BeUInt32 $bytes 4
$fileUsage = Get-BeUInt32 $bytes 128
$fileCount = $bytes[23]
$appletId = Get-BeUInt16 $bytes 20
$maj = $bytes[60]
$min = $bytes[61]
$rev = [char]$bytes[62]

if ($rom -ne $bytes.Length) { throw "romUsage $rom != size $($bytes.Length)" }
if ($appletId -ne [Convert]::ToUInt16("A1C0", 16)) { throw "applet id 0x$($appletId.ToString('X4')) expected 0xA1C0" }
if ($fileCount -ne 2) { throw "fileCount $fileCount expected 2 (mailboxes)" }
if ($fileUsage -ne 1024) { throw "fileUsage $fileUsage expected 1024" }
if ($ram -gt 1536) { throw "ramUsage $ram exceeds RAM_BUDGET 1536" }
if ($ram -gt 800) { throw "ramUsage $ram exceeds RAM_LEGACY_MAX 800" }
if ("$maj.$min.$rev" -ne "0.9.a") { throw "version $maj.$min.$rev expected 0.9.a" }

$sha = (Get-FileHash -Path $out -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "OK: $out ($($bytes.Length) bytes)"
Write-Host "BetaWise: $Tag - Neo Link Chat id=0xA1C0 ver=0.9a mailboxes fileCount=2 fileUsage=1024"
Write-Host "Header: ramUsage=$ram romUsage=$rom"
Write-Host "SHA256: $sha"
Write-Host "Also copied to firmware/main/neo/embedded/NeoLinkChat.OS3KApp"
