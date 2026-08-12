"""Locate firmware images and invoke esptool."""

from __future__ import annotations

import os
import re
import sys
import time
from pathlib import Path

REQUIRED_FILES = (
    "bootloader.bin",
    "partition-table.bin",
    "alpha_smart_neo2_buddy.bin",
    "littlefs.bin",
)

# Friendly labels shown on the install progress bar.
IMAGE_LABELS = {
    "bootloader.bin": "Writing bootloader",
    "partition-table.bin": "Writing partition table",
    "alpha_smart_neo2_buddy.bin": "Writing firmware",
    "littlefs.bin": "Writing file system",
}

# Fraction of the overall bar reserved for write_flash (rest is verify/finish).
_FLASH_SPAN = 0.92

_RE_COMPRESSED = re.compile(r"Compressed\s+(\d+)\s+bytes", re.I)
_RE_WRITING_PCT = re.compile(
    r"Writing\s+at\s+0x[0-9a-fA-F]+\.\.\.\s*\((\d+)\s*%\)",
    re.I,
)
_RE_WROTE = re.compile(r"Wrote\s+\d+\s+bytes", re.I)

# Printed by firmware main.c print_boot_welcome() after services start.
BOOT_MARKER = "AlphaSmart Neo2 Buddy"
CONSOLE_BAUD = 115200

# ESP32-S3 partition layout used by Neo2 Buddy 1.0+
FLASH_ARGS = (
    "--flash_mode",
    "dio",
    "--flash_freq",
    "80m",
    "--flash_size",
    "8MB",
    "0x0",
    "bootloader.bin",
    "0x8000",
    "partition-table.bin",
    "0x10000",
    "alpha_smart_neo2_buddy.bin",
    "0x1C0000",
    "littlefs.bin",
)

# Matches firmware/partitions.csv
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
LITTLEFS_OFFSET = 0x1C0000
LITTLEFS_SIZE = 0x640000


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _frozen_image_dirs() -> list[Path]:
    """Paths used when running as a PyInstaller / frozen app."""
    if not getattr(sys, "frozen", False):
        return []
    dirs: list[Path] = []
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        base = Path(meipass)
        dirs.append(base / "images")
        dirs.append(base)
    exe_dir = Path(sys.executable).resolve().parent
    dirs.append(exe_dir / "images")
    dirs.append(exe_dir)
    # macOS .app: .../Contents/MacOS/<exe> → Resources/images
    if exe_dir.name == "MacOS":
        resources = exe_dir.parent / "Resources"
        dirs.append(resources / "images")
        dirs.append(resources)
    return dirs


def default_image_dirs() -> list[Path]:
    """Candidate folders, first existing complete set wins."""
    here = Path(__file__).resolve().parent
    root = repo_root()
    return [
        *_frozen_image_dirs(),
        here.parent / "images",
        root / "releases" / "1.0.0",
        root / "releases" / "1.0.0-beta.1",
        Path.cwd() / "images",
        Path.cwd(),
    ]


def find_images_dir(explicit: str | Path | None = None) -> Path | None:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.extend(default_image_dirs())
    for path in candidates:
        if images_complete(path):
            return path.resolve()
    return None


def images_complete(folder: Path) -> bool:
    if not folder.is_dir():
        return False
    return all((folder / name).is_file() for name in REQUIRED_FILES)


def missing_images(folder: Path) -> list[str]:
    return [name for name in REQUIRED_FILES if not (folder / name).is_file()]


def image_sizes(folder: Path) -> list[int]:
    """Byte sizes for REQUIRED_FILES (0 if missing). Used to weight progress."""
    sizes: list[int] = []
    for name in REQUIRED_FILES:
        path = folder / name
        try:
            sizes.append(path.stat().st_size if path.is_file() else 0)
        except OSError:
            sizes.append(0)
    return sizes


class FlashProgressTracker:
    """
    Turn esptool stdout into an overall 0..1 progress fraction + status label.

    Weights each image by on-disk size so the long firmware / littlefs writes
    dominate the bar. Parses lines like: Writing at 0x00010000... (42 %)
    """

    def __init__(self, sizes: list[int] | None = None) -> None:
        # Equal weight per image so the long firmware write (many "Writing at… %"
        # lines) moves a meaningful share of the bar. Raw file sizes overweight
        # littlefs erase space relative to how long each stage feels.
        _ = sizes
        n = len(REQUIRED_FILES)
        self._weights = [1.0 / n] * n
        self._buf = ""
        self._image_idx = -1
        self._image_pct = 0
        self._done = [False] * n
        self._fraction = 0.0
        self._status = "Starting install…"

    @property
    def fraction(self) -> float:
        return self._fraction

    @property
    def status(self) -> str:
        return self._status

    def feed(self, text: str) -> list[tuple[float, str]]:
        """Ingest a stdout chunk; return progress updates (may be empty)."""
        if not text:
            return []
        self._buf += text.replace("\r\n", "\n").replace("\r", "\n")
        updates: list[tuple[float, str]] = []
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            line = line.strip()
            if not line:
                continue
            update = self._handle_line(line)
            if update is not None:
                updates.append(update)
        return updates

    def set_phase(self, status: str, fraction: float) -> tuple[float, str]:
        self._status = status
        self._fraction = max(self._fraction, max(0.0, min(1.0, fraction)))
        return self._fraction, self._status

    def _overall(self) -> float:
        done = sum(w for w, finished in zip(self._weights, self._done) if finished)
        if 0 <= self._image_idx < len(REQUIRED_FILES) and not self._done[self._image_idx]:
            done += self._weights[self._image_idx] * (self._image_pct / 100.0)
        return min(_FLASH_SPAN, done * _FLASH_SPAN)

    def _emit(self, status: str | None = None) -> tuple[float, str]:
        if status is not None:
            self._status = status
        # Never move the bar backwards (phase lines vs. first Compressed).
        self._fraction = max(self._fraction, self._overall())
        return self._fraction, self._status

    def _handle_line(self, line: str) -> tuple[float, str] | None:
        lower = line.lower()

        if "connecting" in lower:
            return self.set_phase("Connecting to the board…", 0.02)
        if "uploading stub" in lower or "running stub" in lower or "stub running" in lower:
            return self.set_phase("Preparing the board…", 0.04)
        if "changing baud" in lower:
            return self.set_phase("Preparing the board…", 0.05)
        if "erase_region" in lower or "erasing flash" in lower or "erasing region" in lower:
            return self.set_phase("Wiping portal storage…", 0.08)
        if "configuring flash" in lower or "flash will be erased" in lower:
            return self.set_phase("Erasing flash…", 0.10)

        if _RE_COMPRESSED.search(line):
            # Next image in flash order.
            nxt = self._image_idx + 1
            while nxt < len(REQUIRED_FILES) and self._done[nxt]:
                nxt += 1
            if nxt < len(REQUIRED_FILES):
                self._image_idx = nxt
                self._image_pct = 0
                name = REQUIRED_FILES[nxt]
                label = IMAGE_LABELS.get(name, f"Writing {name}")
                return self._emit(f"{label}…")
            return None

        m = _RE_WRITING_PCT.search(line)
        if m:
            pct = max(0, min(100, int(m.group(1))))
            if self._image_idx < 0:
                self._image_idx = 0
            self._image_pct = pct
            name = REQUIRED_FILES[self._image_idx]
            label = IMAGE_LABELS.get(name, f"Writing {name}")
            return self._emit(f"{label}… {pct}%")

        if _RE_WROTE.search(line) or "hash of data verified" in lower:
            if 0 <= self._image_idx < len(REQUIRED_FILES):
                self._done[self._image_idx] = True
                self._image_pct = 100
                name = REQUIRED_FILES[self._image_idx]
                label = IMAGE_LABELS.get(name, f"Wrote {name}")
                if "hash of data verified" in lower:
                    return self._emit(f"{label} — verified")
                return self._emit(label)
            return None

        if lower.startswith("leaving") or "staying in bootloader" in lower:
            return self.set_phase("Firmware written to the board", _FLASH_SPAN)

        return None


def list_serial_ports() -> list[tuple[str, str]]:
    """Return [(device, description), ...] suitable for a combobox."""
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("pyserial is required (pip install pyserial)") from exc

    ports = []
    for info in list_ports.comports():
        device = info.device or ""
        if not device:
            continue
        desc = info.description or "serial port"
        if info.manufacturer:
            desc = f"{desc} ({info.manufacturer})"
        ports.append((device, f"{device} — {desc}"))
    ports.sort(key=lambda item: item[0].lower())
    return ports


def _run_esptool(args: list[str], *, cwd: Path | None = None, log=print) -> None:
    import esptool

    # Keep the technical command available in details, but lead with plain language.
    if "write_flash" in args:
        log("Writing firmware to the board…\n")
    elif "erase_region" in args:
        log("Erasing flash region…\n")
    log(f"(tool) esptool {' '.join(args)}\n")
    old_cwd = Path.cwd()
    old_argv = sys.argv[:]
    try:
        if cwd is not None:
            os.chdir(cwd)
        sys.argv = ["esptool"] + args
        esptool.main()
    finally:
        sys.argv = old_argv
        os.chdir(old_cwd)


def erase_flash_region(
    port: str,
    offset: int,
    size: int,
    *,
    label: str,
    baud: int = 460800,
    manual_boot: bool = False,
    log=print,
) -> None:
    """Erase a flash region and leave the chip in download mode for the next step."""
    before = "no_reset" if manual_boot else "default_reset"
    after = "no_reset"
    args = [
        "--chip",
        "esp32s3",
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        before,
        "--after",
        after,
        "erase_region",
        f"0x{offset:X}",
        f"0x{size:X}",
    ]
    log(f"Wiping {label} at 0x{offset:X} (size 0x{size:X})…\n")
    _run_esptool(args, log=log)


def erase_nvs(
    port: str,
    *,
    baud: int = 460800,
    manual_boot: bool = False,
    log=print,
) -> None:
    """Erase NVS (Wi‑Fi, portal password, device settings, cloud credentials)."""
    erase_flash_region(
        port,
        NVS_OFFSET,
        NVS_SIZE,
        label="settings (NVS)",
        baud=baud,
        manual_boot=manual_boot,
        log=log,
    )


def erase_littlefs(
    port: str,
    *,
    baud: int = 460800,
    manual_boot: bool = False,
    log=print,
) -> None:
    """Erase the portal LittleFS partition (does not touch NVS or the app)."""
    erase_flash_region(
        port,
        LITTLEFS_OFFSET,
        LITTLEFS_SIZE,
        label="portal storage (LittleFS)",
        baud=baud,
        manual_boot=manual_boot,
        log=log,
    )


def wipe_device_config(
    port: str,
    *,
    baud: int = 460800,
    manual_boot: bool = False,
    log=print,
) -> None:
    """Erase user configuration (NVS) and portal storage (LittleFS)."""
    log(
        "Wiping device configuration: Wi‑Fi, portal password, settings, "
        "and portal file storage…\n"
    )
    erase_nvs(port, baud=baud, manual_boot=manual_boot, log=log)
    # Stay in download mode after the first erase.
    erase_littlefs(port, baud=baud, manual_boot=True, log=log)


def flash_firmware(
    port: str,
    images_dir: Path,
    *,
    baud: int = 460800,
    manual_boot: bool = False,
    wipe_config: bool = False,
    wipe_littlefs: bool | None = None,
    log=print,
) -> None:
    """Flash the buddy firmware. Raises on failure.

    wipe_config: erase NVS (settings) + LittleFS before writing.
    wipe_littlefs: deprecated alias for wipe_config (kept for callers).
    """
    if wipe_littlefs is not None:
        wipe_config = bool(wipe_littlefs) or wipe_config

    if not images_complete(images_dir):
        missing = ", ".join(missing_images(images_dir))
        raise FileNotFoundError(f"Incomplete firmware folder ({images_dir}). Missing: {missing}")

    before = "no_reset" if manual_boot else "default_reset"
    after = "no_reset" if manual_boot else "hard_reset"

    log(f"Installing from: {images_dir}\n")
    if wipe_config:
        wipe_device_config(port, baud=baud, manual_boot=manual_boot, log=log)
        # Board is already in download mode after erase_region --after no_reset.
        before = "no_reset"

    args = [
        "--chip",
        "esp32s3",
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        before,
        "--after",
        after,
        "write_flash",
        *FLASH_ARGS,
    ]

    _run_esptool(args, cwd=images_dir, log=log)


def reset_board(port: str, *, log=print) -> None:
    """Leave download mode / reboot into the app (uses esptool hard_reset)."""
    log("Restarting Buddy…\n")
    try:
        _run_esptool(
            [
                "--chip",
                "esp32s3",
                "-p",
                port,
                "-b",
                "115200",
                "--before",
                "default_reset",
                "--after",
                "hard_reset",
                "chip_id",
            ],
            log=log,
        )
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
        if code != 0:
            raise


def verify_boot(
    port: str,
    *,
    marker: str = BOOT_MARKER,
    timeout_s: float = 45.0,
    console_baud: int = CONSOLE_BAUD,
    reset_first: bool = False,
    log=print,
) -> str:
    """
    Open the UART console and wait for the firmware boot banner.

    Returns the captured text (may be truncated). Raises TimeoutError / OSError.
    More reliable right after flash than the Wi-Fi Python wrapper.
    """
    import serial

    if reset_first:
        reset_board(port, log=log)
        time.sleep(0.4)

    log(f"Waiting for Buddy to start on {port}…\n")
    deadline = time.monotonic() + timeout_s
    buf = ""
    with serial.Serial(port, console_baud, timeout=0.25) as ser:
        try:
            ser.dtr = False
            ser.rts = True
            time.sleep(0.05)
            ser.rts = False
            time.sleep(0.05)
        except Exception:
            pass
        while time.monotonic() < deadline:
            chunk = ser.read(512)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                buf += text
                log(text)
                if marker in buf and "========" in buf[buf.find(marker) : buf.find(marker) + 800]:
                    # Wait briefly for the rest of the welcome block (SSID / Portal lines).
                    extra_deadline = time.monotonic() + 2.0
                    while time.monotonic() < extra_deadline:
                        more = ser.read(512)
                        if more:
                            extra = more.decode("utf-8", errors="replace")
                            buf += extra
                            log(extra)
                        else:
                            time.sleep(0.05)
                        if "Console :" in buf or "Wi-Fi PW:" in buf or "Joined  :" in buf or "Profile :" in buf:
                            break
                    log("\nBuddy started successfully.\n")
                    return buf
            else:
                time.sleep(0.05)
    raise TimeoutError(
        f"Buddy did not respond within {timeout_s:.0f}s. "
        "Press RESET on the board and try again, or reinstall."
    )


def parse_connection_info(boot_text: str) -> dict[str, str]:
    """Extract SoftAP / home-Wi-Fi connection hints from the UART welcome banner."""
    import re

    info: dict[str, str] = {}
    for line in boot_text.splitlines():
        line = line.strip()
        m = re.match(r"Network\s*:\s*(.+)$", line)
        if m:
            info["network"] = m.group(1).strip()
        m = re.match(r"SSID\s*:\s*(.+)$", line)
        if m:
            info["ssid"] = m.group(1).strip()
        m = re.match(r"Wi-Fi PW\s*:\s*(.+)$", line)
        if m:
            info["password"] = m.group(1).strip()
        m = re.match(r"Joined\s*:\s*(.+)$", line)
        if m:
            info["joined"] = m.group(1).strip()
        m = re.match(r"Portal\s*:\s*(.+)$", line)
        if m:
            info["portal"] = m.group(1).strip()
        m = re.match(r"Setup\s*:\s*(.+)$", line)
        if m:
            info["setup"] = m.group(1).strip()
        m = re.match(r"Profile\s*:\s*(.+)$", line)
        if m:
            info["profile"] = m.group(1).strip()
    # Fallbacks from older banners / ESP_LOG lines
    if "portal" not in info:
        m = re.search(r"https?://\d+\.\d+\.\d+\.\d+/?", boot_text)
        if m:
            info["portal"] = m.group(0)
        elif "192.168.4.1" in boot_text or "Direct access" in boot_text or "SoftAP" in boot_text:
            info["portal"] = "http://192.168.4.1/"
    if "ssid" not in info:
        m = re.search(r"(?:hotspot|Direct access hotspot) started ssid=([^\s]+)", boot_text)
        if m:
            info["ssid"] = m.group(1)
    if "network" not in info:
        if "UART-only" in boot_text or "uart-only" in info.get("profile", "").lower():
            info["network"] = "UART-only"
        elif "ssid" in info or "SoftAP" in boot_text or "Direct access" in boot_text:
            info["network"] = "SoftAP / Direct access"
        elif "Home Wi-Fi" in boot_text or "Home network connected" in boot_text:
            info["network"] = "Home Wi-Fi"
    return info


def format_connection_help(info: dict[str, str]) -> str:
    """Human-readable post-install connection instructions."""
    network = info.get("network", "")
    profile = info.get("profile", "")
    lines: list[str] = []
    if "UART" in network or "UART-only" in profile or "uart-only" in profile.lower():
        lines.append("This build is UART-only (no Wi‑Fi portal).")
        lines.append("1. Open a serial terminal at 115200 baud on the programming USB port.")
        lines.append("2. Type login <password>, then help.")
        lines.append("3. Plug the Neo into the Buddy OTG port for backups / applets.")
    elif "SoftAP" in network or "Direct" in network or "recovery" in network.lower() or info.get("ssid"):
        lines.append("Next: connect to Buddy’s own Wi‑Fi network.")
        if info.get("ssid"):
            lines.append(f"1. On your phone or computer, join Wi‑Fi: {info['ssid']}")
        else:
            lines.append("1. On your phone or computer, join the Buddy Wi‑Fi network.")
        if info.get("password"):
            lines.append(f"2. Password: {info['password']}")
        else:
            lines.append("2. Password (new device): neo2buddy")
        portal = info.get("portal") or "http://192.168.4.1/"
        lines.append(f"3. Open a browser to: {portal}")
        if info.get("setup"):
            lines.append(f"   First-time setup page: {info['setup']}")
        else:
            lines.append("   First-time setup page: http://192.168.4.1/setup.html")
    elif "Home" in network or info.get("joined") or (
        info.get("portal") and "192.168.4.1" not in info.get("portal", "")
    ):
        lines.append("Buddy is already on your home Wi‑Fi.")
        if info.get("joined"):
            lines.append(f"Network: {info['joined']}")
        if info.get("portal"):
            lines.append(f"Open the portal in a browser: {info['portal']}")
        else:
            lines.append("Open the portal using the address shown in the details log.")
    else:
        lines.append("Buddy started, but Wi‑Fi details weren’t captured yet.")
        lines.append("Try http://192.168.4.1/ in your browser, or check Show details.")
    return "\n".join(lines)
