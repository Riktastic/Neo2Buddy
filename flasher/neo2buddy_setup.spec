# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for Neo2 Buddy Setup (cross-platform).

Build from a staging directory that contains:
  neo2buddy_flasher/
  images/{bootloader,partition-table,alpha_smart_neo2_buddy,littlefs}.bin

Example:
  pyinstaller --noconfirm neo2buddy_setup.spec
"""

import sys
from pathlib import Path

block_cipher = None
spec_dir = Path(SPECPATH)
images = spec_dir / "images"
datas = []
if images.is_dir():
    datas.append((str(images), "images"))

# Hidden imports used by esptool / serial at runtime
hiddenimports = [
    "serial",
    "serial.tools.list_ports",
    "esptool",
    "reedsolo",
    "bitstring",
    "intelhex",
]

a = Analysis(
    ["run_setup.py"],
    pathex=[str(spec_dir)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

# Windows/Linux: one-folder dist is more reliable with Tk + USB drivers messaging.
# macOS: onedir also produces a .app when BUNDLE is used below.
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="Neo2BuddySetup",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,  # GUI app — no console window
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name="Neo2BuddySetup",
)

if sys.platform == "darwin":
    app = BUNDLE(
        coll,
        name="Neo2BuddySetup.app",
        icon=None,
        bundle_identifier="com.riktastic.neo2buddy.setup",
        info_plist={
            "NSPrincipalClass": "NSApplication",
            "CFBundleName": "Neo2 Buddy Setup",
            "CFBundleDisplayName": "Neo2 Buddy Setup",
            "CFBundleShortVersionString": "1.0.0",
            "NSHighResolutionCapable": True,
        },
    )
