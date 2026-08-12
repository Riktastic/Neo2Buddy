"""ESP-IDF build helpers for custom Neo2 Buddy images."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from .flash import REQUIRED_FILES, repo_root
from .profiles import FeatureSet, fragment_for_preset, write_sdkconfig_fragment


def firmware_dir() -> Path:
    return repo_root() / "firmware"


def find_idf_python() -> Path | None:
    """Prefer the Espressif IDF Python env used by this repo."""
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        cand = Path(env) / "Scripts" / "python.exe"
        if cand.is_file():
            return cand
        cand = Path(env) / "bin" / "python"
        if cand.is_file():
            return cand
    # Common Windows installer layout (matches firmware/idf_env.ps1).
    win = Path(r"C:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe")
    if win.is_file():
        return win
    return None


def find_idf_path() -> Path | None:
    env = os.environ.get("IDF_PATH")
    if env and Path(env).is_dir():
        return Path(env)
    win = Path(r"C:\Espressif\frameworks\esp-idf-v5.3.1")
    if win.is_dir():
        return win
    return None


def find_idf_py() -> Path | None:
    idf = find_idf_path()
    if not idf:
        return None
    script = idf / "tools" / "idf.py"
    return script if script.is_file() else None


def idf_available() -> tuple[bool, str]:
    idf = find_idf_path()
    idf_py = find_idf_py()
    py = find_idf_python()
    if not idf or not idf_py:
        return False, "ESP-IDF not found (set IDF_PATH or install Espressif ESP-IDF 5.3)."
    if not py:
        return False, "IDF Python env not found (set IDF_PYTHON_ENV_PATH)."
    return True, f"IDF at {idf}"


def _sdkconfig_defaults_value(fragment: Path) -> str:
    base = "sdkconfig.defaults"
    # idf.py expects relative paths from the project directory.
    rel = fragment.resolve().relative_to(firmware_dir().resolve()).as_posix()
    return f"{base};{rel}"


def prepare_fragment(
    features: FeatureSet,
    *,
    preset_id: str | None = None,
    export_name: str = "custom",
) -> Path:
    """Use a stock profile fragment when possible; otherwise generate one."""
    if preset_id:
        stock = fragment_for_preset(preset_id)
        if stock is not None and preset_id != "full":
            return stock
        if preset_id == "full":
            # Empty override — builder still writes a marker for export metadata.
            gen = firmware_dir() / "build-custom" / "sdkconfig.defaults.custom"
            write_sdkconfig_fragment(gen, features, label=export_name)
            return gen
    gen = firmware_dir() / "build-custom" / "sdkconfig.defaults.custom"
    write_sdkconfig_fragment(gen, features, label=export_name)
    return gen


def run_idf(
    args: list[str],
    *,
    fragment: Path,
    build_dir: Path,
    log=print,
) -> int:
    py = find_idf_python()
    idf_py = find_idf_py()
    idf_path = find_idf_path()
    if not py or not idf_py or not idf_path:
        raise RuntimeError("ESP-IDF is not available on this machine.")

    env = os.environ.copy()
    env["IDF_PATH"] = str(idf_path)
    env["IDF_PYTHON_ENV_PATH"] = str(py.parent.parent if py.name.startswith("python") else py.parent)

    # Ensure IDF tools are on PATH (best-effort; idf_env.ps1 is the authoritative Windows setup).
    tools = [
        str(py.parent),
        str(idf_path / "tools"),
    ]
    env["PATH"] = os.pathsep.join(tools + [env.get("PATH", "")])

    defaults = _sdkconfig_defaults_value(fragment)
    sdkconfig_path = build_dir / "sdkconfig"
    cmd = [
        str(py),
        str(idf_py),
        "-B",
        str(build_dir),
        "-D",
        f"SDKCONFIG={sdkconfig_path.as_posix()}",
        "-D",
        f"SDKCONFIG_DEFAULTS={defaults}",
        *args,
    ]
    log(f"$ {' '.join(cmd)}\n")
    proc = subprocess.Popen(
        cmd,
        cwd=str(firmware_dir()),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        log(line)
    return proc.wait()


def export_images(
    build_dir: Path,
    export_dir: Path,
    *,
    features: FeatureSet,
    label: str,
) -> Path:
    export_dir.mkdir(parents=True, exist_ok=True)
    mapping = {
        "bootloader.bin": build_dir / "bootloader" / "bootloader.bin",
        "partition-table.bin": build_dir / "partition_table" / "partition-table.bin",
        "alpha_smart_neo2_buddy.bin": build_dir / "alpha_smart_neo2_buddy.bin",
        "littlefs.bin": build_dir / "littlefs.bin",
    }
    missing = [name for name, src in mapping.items() if not src.is_file()]
    if missing:
        raise FileNotFoundError(f"Build incomplete; missing: {', '.join(missing)}")
    for name, src in mapping.items():
        shutil.copy2(src, export_dir / name)
    flash_args = build_dir / "flash_args"
    if flash_args.is_file():
        shutil.copy2(flash_args, export_dir / "flash_args")

    lines = [f"label={label}", f"built={datetime.now().isoformat(timespec='seconds')}"]
    for key, enabled in features.as_dict().items():
        lines.append(f"{key}={'y' if enabled else 'n'}")
    (export_dir / "profile.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return export_dir


def build_and_export(
    features: FeatureSet,
    *,
    export_name: str,
    preset_id: str | None = None,
    fullclean: bool = True,
    log=print,
) -> Path:
    ok, msg = idf_available()
    if not ok:
        raise RuntimeError(msg)

    fragment = prepare_fragment(features, preset_id=preset_id, export_name=export_name)
    build_dir = firmware_dir() / "build-custom" / export_name
    build_dir.mkdir(parents=True, exist_ok=True)

    if fullclean and (build_dir / "CMakeCache.txt").is_file():
        code = run_idf(["fullclean"], fragment=fragment, build_dir=build_dir, log=log)
        if code != 0:
            raise RuntimeError(f"idf.py fullclean failed ({code})")

    # Fresh sdkconfig when switching feature sets.
    sdkconfig = build_dir / "sdkconfig"
    if sdkconfig.is_file() and fullclean:
        sdkconfig.unlink(missing_ok=True)

    code = run_idf(["build"], fragment=fragment, build_dir=build_dir, log=log)
    if code != 0:
        raise RuntimeError(f"idf.py build failed ({code})")

    export_dir = repo_root() / "releases" / "custom" / export_name
    return export_images(build_dir, export_dir, features=features, label=export_name)


def default_export_name(preset_id: str | None, features: FeatureSet) -> str:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    if preset_id:
        return f"{preset_id}-{stamp}"
    if features.is_uart_slim:
        return f"uart-slim-{stamp}"
    return f"custom-{stamp}"
