#!/usr/bin/env python3
"""Backup changed Neo files and pull them to ./backups."""

from pathlib import Path

from neo2buddy_wrapper import Neo2BuddyClient

HOST = "192.168.4.1"
PASSWORD = "neo2buddy"
OUT = Path(__file__).resolve().parent / "backups"


def main() -> None:
    with Neo2BuddyClient(HOST, password=PASSWORD) as buddy:
        print("status:", buddy.status().get("auto_backup_phase"))
        result = buddy.backup_and_pull(OUT, mode="changed")
        print("backup:", result["backup"])
        print("downloaded:")
        for path in result["downloaded"]:
            print(" ", path)


if __name__ == "__main__":
    main()
