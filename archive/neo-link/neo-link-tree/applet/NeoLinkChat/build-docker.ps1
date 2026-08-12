# Rebuild NeoLinkChat.OS3KApp via Docker (wischner/gcc-m68k).
# Requires Docker Desktop running.
# Writes Unix LF files — CRLF Makefiles break GNU make ("No rule to make target 'all\r'").
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$BetaWise = Join-Path $RepoRoot "tools\betawise"
$SrcApplet = Join-Path $RepoRoot "neo-link\applet\NeoLinkChat"
$AppletCommon = Join-Path $RepoRoot "neo-link\applet"
$Protocol = Join-Path $RepoRoot "neo-link\protocol"
$DstApplet = Join-Path $BetaWise "applets\NeoLinkChat"
$Free = Join-Path $BetaWise "os3k\freestanding"

function Write-UnixText {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $normalized = $Content -replace "`r`n", "`n" -replace "`r", "`n"
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, $normalized, $utf8NoBom)
}

if (-not (Test-Path (Join-Path $BetaWise ".git"))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot "tools") | Out-Null
    git clone --depth 1 https://github.com/isotherm/betawise.git $BetaWise
}

New-Item -ItemType Directory -Force -Path $DstApplet | Out-Null
New-Item -ItemType Directory -Force -Path $Free | Out-Null

Copy-Item (Join-Path $SrcApplet "NeoLinkChat.c") $DstApplet -Force
Copy-Item (Join-Path $SrcApplet "neo_link_emit.c") $DstApplet -Force
Copy-Item (Join-Path $SrcApplet "neo_link_inbox.h") $DstApplet -Force
Copy-Item (Join-Path $Protocol "neo_link_limits.h") $DstApplet -Force
Copy-Item (Join-Path $AppletCommon "neo_link_os3k.h") $DstApplet -Force
Copy-Item (Join-Path $AppletCommon "neo_link_applet_guard.h") $DstApplet -Force

Get-ChildItem $DstApplet -File | ForEach-Object {
    $raw = [System.IO.File]::ReadAllText($_.FullName)
    Write-UnixText -Path $_.FullName -Content $raw
}

# Keep applet link set tiny — do NOT link protocol/snprintf (multi-KB BSS).
Write-UnixText -Path (Join-Path $DstApplet "Makefile") -Content @"
OBJ = NeoLinkChat.o
TARGET = NeoLinkChat.OS3KApp

include ../../Makefile.common
"@

# Freestanding headers for toolchains without newlib
Write-UnixText -Path (Join-Path $Free "stdint.h") -Content @'
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
Write-UnixText -Path (Join-Path $Free "stdbool.h") -Content @'
#ifndef _STDBOOL_H
#define _STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#endif
'@
Write-UnixText -Path (Join-Path $Free "stddef.h") -Content @'
#ifndef _STDDEF_H
#define _STDDEF_H
typedef unsigned long size_t;
typedef long ptrdiff_t;
#define NULL ((void*)0)
#endif
'@
Write-UnixText -Path (Join-Path $Free "string.h") -Content @'
#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t num);
void *memset(void *ptr, int value, size_t num);
size_t strlen(const char *str);
char *strncpy(char *dst, const char *src, size_t num);
#endif
'@
Write-UnixText -Path (Join-Path $Free "ctype.h") -Content @'
#ifndef _CTYPE_H
#define _CTYPE_H
static inline int isxdigit(int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}
#endif
'@
Write-UnixText -Path (Join-Path $Free "stdarg.h") -Content @'
#ifndef _STDARG_H
#define _STDARG_H
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v,t) __builtin_va_arg(v,t)
#endif
'@
Write-UnixText -Path (Join-Path $Free "stdio.h") -Content @'
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
int snprintf(char *str, size_t size, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
#endif
'@

# Patch Makefile.common once to prefer freestanding includes (keep LF)
$mc = Join-Path $BetaWise "Makefile.common"
$mcText = [System.IO.File]::ReadAllText($mc)
if ($mcText -notmatch 'os3k/freestanding') {
    $mcText = $mcText -replace '-Os -I"\$\(TOP\)/os3k"', '-Os -I"$(TOP)/os3k/freestanding" -I"$(TOP)/os3k"'
}
Write-UnixText -Path $mc -Content $mcText

# versiongen without python/tags (must be LF shell script despite .py name)
Write-UnixText -Path (Join-Path $BetaWise "os3k\versiongen.py") -Content @'
#!/bin/sh
cat <<EOF
#define BETAWISE_VERSION_MAJOR 0
#define BETAWISE_VERSION_MINOR 9
#define BETAWISE_VERSION_REVISION " "
EOF
'@

if ($BetaWise -match '^([A-Za-z]):\\(.*)$') {
    $mount = "$($Matches[1].ToLower()):/$($Matches[2] -replace '\\','/')"
} else {
    throw "Unexpected BetaWise path: $BetaWise"
}

# LF shell script inside the mount — avoids PowerShell CRLF breaking `docker ... bash -c`
Write-UnixText -Path (Join-Path $DstApplet "docker-build.sh") -Content @'
#!/bin/sh
set -e
cd /src
for f in os3k/versiongen.py applets/NeoLinkChat/Makefile applets/HelloWorld/Makefile Makefile.common; do
  if [ -f "$f" ]; then
    sed -i "s/\r$//" "$f"
  fi
done
chmod +x os3k/versiongen.py
make -C os3k all
make -C applets/HelloWorld clean all
make -C applets/NeoLinkChat clean all
if cmp -s applets/HelloWorld/HelloWorld.OS3KApp applets/NeoLinkChat/NeoLinkChat.OS3KApp; then
  echo "OK: NeoLinkChat matches BetaWise HelloWorld (byte-identical)"
else
  echo "NOTE: NeoLinkChat differs from HelloWorld (isolation / bring-up build)"
  ls -la applets/HelloWorld/HelloWorld.OS3KApp applets/NeoLinkChat/NeoLinkChat.OS3KApp
fi
ls -la applets/NeoLinkChat/NeoLinkChat.OS3KApp
'@

docker pull wischner/gcc-m68k:latest | Out-Host
docker run --rm -v "${mount}:/src" -w /src wischner/gcc-m68k:latest sh /src/applets/NeoLinkChat/docker-build.sh
if (-not (Test-Path (Join-Path $DstApplet "NeoLinkChat.OS3KApp"))) {
    throw "Docker build did not produce NeoLinkChat.OS3KApp"
}

Copy-Item (Join-Path $DstApplet "NeoLinkChat.OS3KApp") (Join-Path $SrcApplet "NeoLinkChat.OS3KApp") -Force
$embedDir = Join-Path $RepoRoot "firmware\main\neo\embedded"
New-Item -ItemType Directory -Force -Path $embedDir | Out-Null
Copy-Item (Join-Path $DstApplet "NeoLinkChat.OS3KApp") (Join-Path $embedDir "NeoLinkChat.OS3KApp") -Force
$out = Join-Path $SrcApplet "NeoLinkChat.OS3KApp"
Write-Host "OK: $out ($((Get-Item $out).Length) bytes)"
Write-Host "OK: $(Join-Path $embedDir 'NeoLinkChat.OS3KApp') (firmware embed)"
$sha = (Get-FileHash -Path $out -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "SHA256: $sha"

# Header sanity check — ramUsage must stay within Neo budget (see docs/os3k-applet.md)
$hdr = [IO.File]::ReadAllBytes($out)
if ($hdr.Length -lt 132) { throw "OS3KApp too small" }
function Get-BeUInt32([byte[]]$buf, [int]$off) {
    return [uint32](([uint32]$buf[$off] -shl 24) -bor ([uint32]$buf[$off + 1] -shl 16) -bor ([uint32]$buf[$off + 2] -shl 8) -bor [uint32]$buf[$off + 3])
}
$ram = Get-BeUInt32 $hdr 8
$fileUsage = Get-BeUInt32 $hdr 128
Write-Host "Header: ramUsage=$ram fileUsage=$fileUsage"
if ($ram -gt 1536) {
    throw "ramUsage $ram exceeds NEO_LINK_APPLET_RAM_BUDGET (1536) - do not link protocol.c on Neo"
}
$maj = $hdr[60]
$min = $hdr[61]
$rev = $hdr[62]
Write-Host "Version: $maj.$min.$([char]$rev) (BetaWise HelloWorld header)"
if ($ram -gt 800) {
    throw "ramUsage $ram exceeds NEO_LINK_APPLET_RAM_LEGACY_MAX (800) - shrink BSS before shipping"
}
# Mailboxes temporarily disabled — fileCount/fileUsage must be 0 (see NEO_LINK_APPLET_MAILBOXES_ENABLED)
if ($fileUsage -ne 0) {
    throw "fileUsage $fileUsage != 0 (mailbox bring-up build)"
}
if ($hdr[23] -ne 0) {
    throw "fileCount must be 0 for mailbox-off test build"
}

$verify = Join-Path $RepoRoot "tools\verify_neo_link_applet.py"
if (Test-Path $verify) {
    python $verify
    if ($LASTEXITCODE -ne 0) { throw "verify_neo_link_applet.py failed" }
}
