# Neo2 Buddy Python wrapper

HTTP client and CLI for a Neo2 Buddy on your network. Same `/api/v1` surface as
the web portal: backups, Neo USB ops, local files, Wi‑Fi, cloud sync, BLE text
relay, logs, and diagnostics.

| | |
|--|--|
| **Folder** | `python-wrapper/` |
| **Import** | `neo2buddy_wrapper` |
| **CLI** | `neo2buddy` or `neo2buddy-wrapper` |
| **Default AP** | `http://192.168.4.1` (password `neo2buddy` until you change it) |

## Install

```powershell
cd python-wrapper
pip install -e .
```

## Library quick start

```python
from neo2buddy_wrapper import Neo2BuddyClient

with Neo2BuddyClient("192.168.4.1", password="neo2buddy") as buddy:
    # Backup changed AlphaWord files, return Neo to keyboard, then download
    result = buddy.backup_and_pull("./backups", mode="changed")
    print(result["downloaded"])

    # Cloud sync (WebDAV / S3-compatible)
    buddy.set_sync_config(
        provider="webdav",
        enabled=True,
        endpoint="https://cloud.example.com/remote.php/dav/files/user/",
        folder="neo-backups",
        username="user",
        secret="app-password",
    )
    buddy.sync_test()
    buddy.sync_run(wait=True)

    # Neo document I/O
    text = buddy.read_file_text(1)  # AlphaWord slot 1
    buddy.write_file_text(2, "Hello from Python\n")
```

## CLI

```powershell
neo2buddy status
neo2buddy backup
neo2buddy backup --mode all --pull ./backups
neo2buddy pull ./backups
neo2buddy list-backups
neo2buddy list-neo

neo2buddy settings
neo2buddy settings --set-label classroom-a --auto-backup on

neo2buddy wifi status
neo2buddy wifi scan
neo2buddy wifi connect "MyNetwork" "password"

neo2buddy sync config
neo2buddy sync config --provider webdav --enabled on --endpoint https://... --folder neo
neo2buddy sync test
neo2buddy sync run

neo2buddy neo info
neo2buddy neo applets
neo2buddy neo read 1
neo2buddy neo write 2 document.txt
neo2buddy neo install applet.os3kapp

# BLE: portal text → paired host (not Neo key passthrough)
neo2buddy ble status
neo2buddy ble preview "Hello world"
neo2buddy ble send

neo2buddy logs --limit 30
neo2buddy neo debug
```

Examples: [`examples/`](examples/).

## Notes

- Backup / scan / read / write on the Neo interrupt keyboard mode (same as the portal).
- BLE send is wrapper/portal text only; Neo keystrokes stay on USB.

## API surface

### Auth & device

| Method | Purpose |
|--------|---------|
| `login()` / `logout()` / `refresh_token()` | Portal auth |
| `status()` | Device + Neo USB + auto-backup |
| `get_logs(limit)` | Portal log buffer |
| `get_settings()` / `set_settings(**)` | Device configuration |
| `get_onboarding()` / `post_onboarding(**)` | First-run setup (no auth) |

### Wi‑Fi & storage

| Method | Purpose |
|--------|---------|
| `get_wifi()` / `scan_wifi()` / `configure_wifi()` / `confirm_wifi()` | Wi‑Fi |
| `sd_status()` / `sd_format()` | SD card |

### Cloud sync

| Method | Purpose |
|--------|---------|
| `get_sync_config()` / `set_sync_config(**)` | WebDAV or S3 destination |
| `sync_test()` / `sync_run()` / `wait_sync()` | Upload local backups |

### BLE & keyboard capture

| Method | Purpose |
|--------|---------|
| `get_ble()` / `ble_pairing()` / `ble_preview()` / `ble_send()` / `ble_cancel()` | BLE HID relay |
| `keyboard_recent()` / `keyboard_raw()` / `keyboard_clear()` | Neo HID capture (USB) |

### Local backup files (on buddy)

| Method | Purpose |
|--------|---------|
| `list_backups()` / `view_backup()` / `upload_backup()` | List, read, upload |
| `download_backup()` / `pull_backups()` / `delete_backup()` / `rename_backup()` | Download, delete, rename |

### Neo USB

| Method | Purpose |
|--------|---------|
| `backup_now()` / `backup_all()` / `backup_file()` | Backup to buddy storage |
| `backup_and_pull(dest)` | Backup then download to PC |
| `list_neo_files()` / `list_applets()` | Scan Neo contents |
| `read_file_text()` / `write_file_text()` / `clear_file()` | Per-slot I/O |
| `write_file_by_target()` / `clear_file_by_target()` | By name or file-space |
| `install_applet()` / `remove_applet()` / `download_applet()` | SmartApplets |
| `neo_info()` / `neo_mode()` / `neo_debug()` | Diagnostics |
| `space_available()` / `space_used()` | Capacity |
| `get_applet_settings()` / `set_applet_settings()` | Applet settings JSON |
| `rescan()` / `restart_keyboard()` | USB helpers |
