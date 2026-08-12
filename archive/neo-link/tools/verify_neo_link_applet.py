#!/usr/bin/env python3
"""
Verify NeoLinkChat.OS3KApp and applet sources stay within neo_link_limits.h.

Exit 0 on success, 1 on failure. Called from build-docker.ps1 after link.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LIMITS_H = REPO / "neo-link" / "protocol" / "neo_link_limits.h"
APPLET_C = REPO / "neo-link" / "applet" / "NeoLinkChat" / "NeoLinkChat.c"
EMIT_C = REPO / "neo-link" / "applet" / "NeoLinkChat" / "neo_link_emit.c"
MAKEFILE = REPO / "tools" / "betawise" / "applets" / "NeoLinkChat" / "Makefile"
OS3KAPP = REPO / "neo-link" / "applet" / "NeoLinkChat" / "NeoLinkChat.OS3KApp"
EMBED = REPO / "firmware" / "main" / "neo" / "embedded" / "NeoLinkChat.OS3KApp"


def parse_limits(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    out: dict[str, int] = {}
    for m in re.finditer(r"#define\s+(NEO_LINK_[A-Z0-9_]+)\s+(\d+)", text):
        out[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"#define\s+(NEO_LINK_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)", text):
        out[m.group(1)] = int(m.group(2), 16)
    for m in re.finditer(r"#define\s+(NEO_LINK_APPLET_VERSION_MAJOR)\s+(\d+)", text):
        out[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"#define\s+(NEO_LINK_APPLET_VERSION_MINOR)\s+(\d+)", text):
        out[m.group(1)] = int(m.group(2))
    rev = re.search(r'#define\s+NEO_LINK_APPLET_VERSION_REV\s+\'(.)\'', text)
    if rev:
        out["NEO_LINK_APPLET_VERSION_REV"] = ord(rev.group(1))
    mb = re.search(r"#define\s+NEO_LINK_APPLET_MAILBOXES_ENABLED\s+(\d+)", text)
    if mb:
        out["NEO_LINK_APPLET_MAILBOXES_ENABLED"] = int(mb.group(1))
    return out


def read_u32_be(buf: bytes, off: int) -> int:
    return struct.unpack(">I", buf[off : off + 4])[0]


def verify_os3kapp(path: Path, lim: dict[str, int]) -> list[str]:
    errs: list[str] = []
    if not path.is_file():
        return [f"missing {path}"]
    hdr = path.read_bytes()
    if len(hdr) < 132:
        return [f"{path.name}: file too small"]
    if read_u32_be(hdr, 0) != 0xC0FFEEAD:
        errs.append(f"{path.name}: bad signature")
    if read_u32_be(hdr, len(hdr) - 4) != 0xCAFEFEED:
        errs.append(f"{path.name}: bad footer signature")
    ram = read_u32_be(hdr, 8)
    rom = read_u32_be(hdr, 4)
    if rom != len(hdr):
        errs.append(f"{path.name}: romUsage={rom} != file size {len(hdr)}")
    applet_id = struct.unpack(">H", hdr[20:22])[0]
    file_count = hdr[23]
    file_usage = read_u32_be(hdr, 128)
    maj, min, rev = hdr[60], hdr[61], hdr[62]

    budget = lim.get("NEO_LINK_APPLET_RAM_BUDGET", 1536)
    legacy = lim.get("NEO_LINK_APPLET_RAM_LEGACY_MAX", 800)
    if ram > budget:
        errs.append(f"{path.name}: ramUsage={ram} > RAM_BUDGET={budget}")
    if ram > legacy:
        errs.append(
            f"{path.name}: ramUsage={ram} > RAM_LEGACY_MAX={legacy} "
            "(buddy will auto-replace; shrink BSS or you risk Adresfout)"
        )
    mailboxes = lim.get("NEO_LINK_APPLET_MAILBOXES_ENABLED", 1)
    expect_fc = 2 if mailboxes else 0
    expect_fu = lim.get("NEO_LINK_APPLET_FILE_USAGE", 1024) if mailboxes else 0
    if file_usage != expect_fu:
        errs.append(f"{path.name}: fileUsage={file_usage} expected {expect_fu}")
    if file_count != expect_fc:
        errs.append(f"{path.name}: fileCount={file_count} expected {expect_fc}")
    if applet_id != lim.get("NEO_LINK_APPLET_ID", 0xA1A0):
        errs.append(f"{path.name}: applet id 0x{applet_id:04x}")
    if mailboxes:
        if maj != lim.get("NEO_LINK_APPLET_VERSION_MAJOR", 0):
            errs.append(f"{path.name}: version major {maj}")
        if min != lim.get("NEO_LINK_APPLET_VERSION_MINOR", 0):
            errs.append(f"{path.name}: version minor {min}")
        if rev != lim.get("NEO_LINK_APPLET_VERSION_REV", ord("f")):
            errs.append(f"{path.name}: version rev {chr(rev)!r}")
    if rom < 256 or rom > 65535:
        errs.append(f"{path.name}: suspicious romUsage={rom}")
    print(
        f"OK {path.name}: ram={ram} rom={rom} fileUsage={file_usage} "
        f"ver={maj}.{min}.{chr(rev)} id=0x{applet_id:04X}"
    )
    return errs


def verify_makefile(path: Path, lim: dict[str, int]) -> list[str]:
    errs: list[str] = []
    if not path.is_file():
        return [f"missing Makefile {path}"]
    text = path.read_text(encoding="utf-8")
    if "neo_link_protocol" in text or "neo_link_snprintf" in text:
        errs.append("Makefile links forbidden protocol/snprintf on Neo")
    objs = re.findall(r"(\w+\.o)", text)
    max_objs = lim.get("NEO_LINK_APPLET_MAX_LINK_OBJECTS", 2)
    if not lim.get("NEO_LINK_APPLET_MAILBOXES_ENABLED", 1):
        max_objs = 1
    applet_objs = [o for o in objs if o not in ("os3k.o",)]
    if len(set(applet_objs)) > max_objs:
        errs.append(f"Makefile links {applet_objs} — max {max_objs} applet objects")
    if "NeoLinkChat.o" not in text:
        errs.append("Makefile must link NeoLinkChat.o")
    if lim.get("NEO_LINK_APPLET_MAILBOXES_ENABLED", 1) and "neo_link_emit.o" not in text:
        errs.append("Makefile must link neo_link_emit.o when mailboxes enabled")
    return errs


def verify_sources(lim: dict[str, int]) -> list[str]:
    errs: list[str] = []
    for path in (APPLET_C, EMIT_C):
        if not path.is_file():
            errs.append(f"missing {path}")
            continue
        text = path.read_text(encoding="utf-8")
        if path == APPLET_C and "neo_link_applet_guard.h" not in text:
            if lim.get("NEO_LINK_APPLET_MAILBOXES_ENABLED", 1):
                errs.append("NeoLinkChat.c must include neo_link_applet_guard.h")
        if re.search(r"SetCursor\([^,]+,\s*0\b", text):
            errs.append(f"{path.name}: SetCursor with col 0")
        if re.search(r"ClearRowCols\([^,]+,\s*0\b", text):
            errs.append(f"{path.name}: ClearRowCols with col 0")
        if re.search(r"PutStringCentered\(0\b", text):
            errs.append(f"{path.name}: PutStringCentered row 0")
        if '#include "neo_link_protocol.h"' in text or re.search(r"#include\s+[\"<]neo_link_protocol", text):
            errs.append(f"{path.name}: must not include protocol on Neo")
        if re.search(r"char\s+\w+\[(?:256|512|1024|2048)\]", text):
            errs.append(f"{path.name}: char[>=256] — use BSS static or stream (see STACK_BUF_MAX)")
    return errs


def main() -> int:
    lim = parse_limits(LIMITS_H)
    required = [
        "NEO_LINK_APPLET_RAM_BUDGET",
        "NEO_LINK_APPLET_RAM_LEGACY_MAX",
        "NEO_LINK_APPLET_FILE_USAGE",
        "NEO_LINK_APPLET_ID",
    ]
    missing = [k for k in required if k not in lim]
    if missing:
        print("limits.h missing:", ", ".join(missing))
        return 1

    errs: list[str] = []
    errs.extend(verify_sources(lim))
    errs.extend(verify_makefile(MAKEFILE, lim))
    errs.extend(verify_os3kapp(OS3KAPP, lim))
    if EMBED.is_file() and EMBED.resolve() != OS3KAPP.resolve():
        if OS3KAPP.read_bytes() != EMBED.read_bytes():
            errs.append("embedded NeoLinkChat.OS3KApp differs from applet build output")

    if errs:
        print("VERIFY FAILED:")
        for e in errs:
            print(" -", e)
        return 1
    print("verify_neo_link_applet: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
