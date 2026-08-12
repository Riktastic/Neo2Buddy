#!/usr/bin/env python3
"""Prepare firmware-web for SPIFFS: drop dead assets, gzip large files for SoftAP.

Large assets are stored as .gz only (no plain twin) to save SPIFFS space.
The HTTP server serves those with Content-Encoding: gzip.
"""
from __future__ import annotations

import gzip
import shutil
import sys
from pathlib import Path

SKIP_NAMES = {
    "styles.css",
    "login.css",
    "app.js.bak",
    "portal.css.bak",
}
SKIP_SUFFIXES = {".md", ".ps1", ".map", ".bak"}
GZIP_MIN_BYTES = 2048
GZIP_NAMES = {
    "js/app.js",
    "js/core.js",
    "js/settings.js",
    "js/typing.js",
    "js/demo.js",
    "css/portal.css",
    "index.html",
    "typing.html",
    "setup.html",
}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: pack_firmware_web.py SRC_DIR DST_DIR", file=sys.stderr)
        return 2
    src = Path(sys.argv[1]).resolve()
    dst = Path(sys.argv[2]).resolve()
    if not src.is_dir():
        print(f"missing source: {src}", file=sys.stderr)
        return 1

    if dst.exists():
        shutil.rmtree(dst)

    def ignore(_dir: str, names: list[str]) -> set[str]:
        skip: set[str] = set()
        for name in names:
            if name in SKIP_NAMES:
                skip.add(name)
            elif any(name.endswith(suf) for suf in SKIP_SUFFIXES):
                skip.add(name)
        return skip

    shutil.copytree(src, dst, ignore=ignore)

    for rel in sorted(GZIP_NAMES):
        path = dst / rel
        if not path.is_file():
            continue
        raw = path.read_bytes()
        if len(raw) < GZIP_MIN_BYTES:
            continue
        gz_path = Path(str(path) + ".gz")
        with gzip.open(gz_path, "wb", compresslevel=9) as gf:
            gf.write(raw)
        plain_size = len(raw)
        gz_size = gz_path.stat().st_size
        path.unlink()  # SPIFFS keeps .gz only — portal serves with Content-Encoding
        print(f"gzip-only {rel}: {plain_size} -> {gz_size} bytes (saved {plain_size - gz_size})")

    print(f"packed web -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
