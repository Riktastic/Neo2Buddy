#!/usr/bin/env python3
"""Create a minimal SPIFFS tree for UART-only firmware profiles."""

from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <out_dir>", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    # Keep a tiny marker so mounts succeed and tools can detect UART-only packs.
    (out / "README.txt").write_text(
        "Neo2 Buddy UART-only profile — no web portal assets.\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
