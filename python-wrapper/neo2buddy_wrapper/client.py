"""
Remote client for AlphaSmart Neo2 Buddy.

Talks to the buddy over its local HTTP portal API (default
http://192.168.4.1). Covers the same /api/v1 surface as the web portal:
backups, Neo file/applet management, local file storage, Wi-Fi, cloud sync,
BLE keyboard relay, logs, and diagnostics.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any, Callable, Mapping

import requests
from urllib.parse import urljoin

from .exceptions import ApiError, AuthError, BusyError, Neo2BuddyError

ALPHAWORD_APPLET_ID = 0xA000


class Neo2BuddyClient:
    """HTTP client for a Neo2 Buddy on the local network."""

    def __init__(
        self,
        host: str = "192.168.4.1",
        *,
        password: str = "neo2buddy",
        scheme: str = "http",
        port: int | None = None,
        timeout: float = 60.0,
        session: requests.Session | None = None,
    ) -> None:
        host = host.strip().rstrip("/")
        if "://" in host:
            base = host if host.endswith("/") else host + "/"
        else:
            authority = f"{host}:{port}" if port else host
            base = f"{scheme}://{authority}/"
        self.base_url = base
        self.api_base = urljoin(base, "api/v1/")
        self.password = password
        self.timeout = timeout
        self._session = session or requests.Session()
        self._token: str | None = None

    # ------------------------------------------------------------------
    # Auth / transport
    # ------------------------------------------------------------------

    @property
    def token(self) -> str | None:
        return self._token

    def login(self, password: str | None = None) -> str:
        """Authenticate and store a bearer token. Returns the token."""
        pw = self.password if password is None else password
        data = self._request(
            "POST",
            "login",
            json_body={"password": pw},
            auth=False,
        )
        token = data.get("token") if isinstance(data, dict) else None
        if not token:
            raise AuthError("Login response missing token")
        self._token = token
        self.password = pw
        return token

    def refresh_token(self) -> str:
        """Rotate the bearer token (POST /token/refresh)."""
        data = self._request("POST", "token/refresh", json_body={}, auth=True)
        token = data.get("token") if isinstance(data, dict) else None
        if not token:
            raise AuthError("Token refresh response missing token")
        self._token = token
        return token

    def ensure_login(self) -> None:
        if not self._token:
            self.login()

    def logout(self) -> None:
        try:
            if self._token:
                self._request("POST", "logout", json_body={})
        finally:
            self._token = None

    def close(self) -> None:
        self.logout()
        self._session.close()

    def __enter__(self) -> "Neo2BuddyClient":
        self.ensure_login()
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _url(self, path: str) -> str:
        return urljoin(self.api_base, path.lstrip("/"))

    def _headers(self, *, auth: bool, extra: Mapping[str, str] | None = None) -> dict[str, str]:
        headers = {"Accept": "application/json"}
        if auth:
            if not self._token:
                raise AuthError("Not logged in — call login() first")
            headers["Authorization"] = f"Bearer {self._token}"
        if extra:
            headers.update(extra)
        return headers

    def _raw_response(
        self,
        method: str,
        path: str,
        *,
        json_body: Any = None,
        data: bytes | None = None,
        params: Mapping[str, Any] | None = None,
        headers: Mapping[str, str] | None = None,
        auth: bool = True,
        timeout: float | None = None,
    ) -> requests.Response:
        if auth:
            self.ensure_login()
        hdrs = self._headers(auth=auth, extra=headers)
        try:
            return self._session.request(
                method,
                self._url(path),
                headers=hdrs,
                json=json_body if data is None else None,
                data=data,
                params=params,
                timeout=timeout or self.timeout,
            )
        except requests.RequestException as exc:
            raise ApiError(f"Request failed: {exc}") from exc

    def _request(
        self,
        method: str,
        path: str,
        *,
        json_body: Any = None,
        data: bytes | None = None,
        params: Mapping[str, Any] | None = None,
        headers: Mapping[str, str] | None = None,
        auth: bool = True,
        raw: bool = False,
        timeout: float | None = None,
    ) -> Any:
        resp = self._raw_response(
            method,
            path,
            json_body=json_body,
            data=data,
            params=params,
            headers=headers,
            auth=auth,
            timeout=timeout,
        )

        if resp.status_code == 401:
            self._token = None
            raise AuthError("Unauthorized — check password or re-login")
        if resp.status_code == 409:
            body = resp.text or ""
            if "auto_backup_busy" in body:
                raise BusyError("A backup is already running on the buddy")
            if "sync_busy" in body:
                raise BusyError("A cloud sync upload is already running")
        if resp.status_code >= 400:
            raise ApiError(
                f"{method} {path} failed ({resp.status_code})",
                status_code=resp.status_code,
                body=resp.text,
            )
        if raw:
            return resp.content
        if not resp.content:
            return None
        ctype = resp.headers.get("Content-Type", "")
        if "json" in ctype or (resp.text[:1] in "{["):
            try:
                return resp.json()
            except ValueError as exc:
                raise ApiError("Invalid JSON response", body=resp.text) from exc
        return resp.text

    def _request_text(
        self,
        method: str,
        path: str,
        *,
        params: Mapping[str, Any] | None = None,
        timeout: float | None = None,
    ) -> str:
        resp = self._raw_response(method, path, params=params, timeout=timeout)
        if resp.status_code == 401:
            self._token = None
            raise AuthError("Unauthorized — check password or re-login")
        if resp.status_code >= 400:
            raise ApiError(
                f"{method} {path} failed ({resp.status_code})",
                status_code=resp.status_code,
                body=resp.text,
            )
        return resp.text

    def _download_to_path(
        self,
        path: str,
        dest: str | Path,
        *,
        params: Mapping[str, Any] | None = None,
    ) -> Path:
        dest_path = Path(dest)
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        content = self._request("GET", path, params=params, raw=True)
        dest_path.write_bytes(content)
        return dest_path

    # ------------------------------------------------------------------
    # Status / device
    # ------------------------------------------------------------------

    def status(self) -> dict[str, Any]:
        """Device + Neo USB status (includes auto_backup_* fields)."""
        data = self._request("GET", "status")
        if not isinstance(data, dict):
            raise ApiError("Unexpected status payload")
        return data

    def usb_status(self) -> dict[str, Any]:
        """Alias for status() — matches legacy /usb/status path."""
        return self.status()

    def get_logs(self, limit: int = 50) -> list[dict[str, Any]]:
        """Recent portal log lines (GET /logs)."""
        data = self._request("GET", "logs", params={"limit": limit})
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected logs payload")

    def wait_until_ready(self, timeout: float = 30.0, poll: float = 1.0) -> dict[str, Any]:
        """Poll until Neo is connected (keyboard or comms) or timeout."""
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            last = self.status()
            if last.get("usb_connected") or last.get("usb_neo_ready") or last.get("usb_keyboard_active"):
                return last
            if last.get("usb_flipping") or last.get("auto_backup_busy"):
                time.sleep(poll)
                continue
            time.sleep(poll)
        raise Neo2BuddyError(f"Neo not ready within {timeout:.0f}s (last={last})")

    # ------------------------------------------------------------------
    # Onboarding (no auth while setup incomplete)
    # ------------------------------------------------------------------

    def get_onboarding(self) -> dict[str, Any]:
        data = self._request("GET", "onboarding", auth=False)
        if not isinstance(data, dict):
            raise ApiError("Unexpected onboarding payload")
        return data

    def post_onboarding(self, **fields: Any) -> dict[str, Any]:
        """First-run setup (works without login while onboarding incomplete)."""
        data = self._request("POST", "onboarding", json_body=fields, auth=False)
        return data if isinstance(data, dict) else {"ok": True}

    # ------------------------------------------------------------------
    # Settings
    # ------------------------------------------------------------------

    def get_settings(self) -> dict[str, Any]:
        data = self._request("GET", "settings")
        if not isinstance(data, dict):
            raise ApiError("Unexpected settings payload")
        return data

    def set_settings(self, **fields: Any) -> dict[str, Any]:
        """Update settings fields (device_name, neo_label, wifi_*, network_mode, …)."""
        data = self._request("POST", "settings", json_body=fields)
        return data if isinstance(data, dict) else {"ok": True}

    def set_auto_backup(self, enabled: bool) -> dict[str, Any]:
        return self.set_settings(auto_backup_on_connect=bool(enabled))

    def set_neo_label(self, label: str) -> dict[str, Any]:
        return self.set_settings(neo_label=label)

    # ------------------------------------------------------------------
    # Wi-Fi
    # ------------------------------------------------------------------

    def get_wifi(self) -> dict[str, Any]:
        data = self._request("GET", "wifi")
        if not isinstance(data, dict):
            raise ApiError("Unexpected wifi payload")
        return data

    def scan_wifi(self, *, auth: bool | None = None) -> list[dict[str, Any]]:
        """Scan for access points. Auth optional during first-run onboarding."""
        if auth is None:
            auth = not self.get_onboarding().get("onboarding_complete", True)
        data = self._request("GET", "wifi/scan", auth=auth)
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected wifi scan payload")

    def configure_wifi(
        self,
        ssid: str,
        password: str = "",
        *,
        dhcp: bool = True,
        ip: str = "",
        netmask: str = "",
        gateway: str = "",
        dns: str = "",
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"ssid": ssid, "password": password, "dhcp": dhcp}
        if ip:
            body["ip"] = ip
        if netmask:
            body["netmask"] = netmask
        if gateway:
            body["gateway"] = gateway
        if dns:
            body["dns"] = dns
        data = self._request("POST", "wifi", json_body=body)
        return data if isinstance(data, dict) else {"ok": True}

    def confirm_wifi(self) -> dict[str, Any]:
        """Confirm STA connection after portal reconnects (POST /wifi/confirm)."""
        data = self._request("POST", "wifi/confirm", json_body={})
        return data if isinstance(data, dict) else {"ok": True}

    # ------------------------------------------------------------------
    # SD card
    # ------------------------------------------------------------------

    def sd_status(self) -> dict[str, Any]:
        data = self._request("GET", "sd/status")
        if not isinstance(data, dict):
            raise ApiError("Unexpected sd status payload")
        return data

    def sd_format(self) -> dict[str, Any]:
        """Start async SD format (destructive)."""
        data = self._request("POST", "sd/format", json_body={})
        return data if isinstance(data, dict) else {"started": True}

    # ------------------------------------------------------------------
    # Cloud sync (WebDAV / S3)
    # ------------------------------------------------------------------

    def get_sync_config(self) -> dict[str, Any]:
        data = self._request("GET", "sync/config")
        if not isinstance(data, dict):
            raise ApiError("Unexpected sync config payload")
        return data

    def set_sync_config(self, **fields: Any) -> dict[str, Any]:
        """
        Update cloud sync destination (PUT /sync/config).

        Fields: provider (none|webdav|s3), enabled, endpoint, folder/path,
        bucket, region, username, secret (write-only; never returned on GET).
        """
        data = self._request("PUT", "sync/config", json_body=fields)
        return data if isinstance(data, dict) else {"ok": True}

    def sync_test(self) -> dict[str, Any]:
        """Test cloud credentials/connectivity."""
        data = self._request("POST", "sync/test", json_body={})
        return data if isinstance(data, dict) else {"ok": True}

    def sync_run(self, *, wait: bool = False, timeout: float = 600.0, poll: float = 2.0) -> dict[str, Any]:
        """Upload all local backup files to configured cloud destination."""
        data = self._request("POST", "sync/run", json_body={})
        if not wait:
            return data if isinstance(data, dict) else {"started": True}
        return self.wait_sync(timeout=timeout, poll=poll)

    def wait_sync(self, *, timeout: float = 600.0, poll: float = 2.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            cfg = self.get_sync_config()
            status = cfg.get("status") if isinstance(cfg.get("status"), dict) else {}
            last = status
            if not status.get("busy"):
                if status.get("last_ok") is False and status.get("last_message"):
                    raise Neo2BuddyError(f"Cloud sync failed: {status.get('last_message')}")
                return cfg
            time.sleep(poll)
        raise Neo2BuddyError(f"Cloud sync timed out after {timeout:.0f}s (last={last})")

    # ------------------------------------------------------------------
    # BLE HID relay
    # ------------------------------------------------------------------

    def get_ble(self) -> dict[str, Any]:
        data = self._request("GET", "ble")
        if not isinstance(data, dict):
            raise ApiError("Unexpected ble payload")
        return data

    def ble_pairing(self, enabled: bool = True) -> dict[str, Any]:
        data = self._request("POST", "ble/pairing", json_body={"enabled": enabled})
        return data if isinstance(data, dict) else {"pairing": enabled}

    def ble_preview(self, text: str) -> dict[str, Any]:
        data = self._request("POST", "ble/preview", json_body={"text": text})
        if not isinstance(data, dict):
            raise ApiError("Unexpected ble preview payload")
        return data

    def ble_send(self) -> dict[str, Any]:
        data = self._request("POST", "ble/send", json_body={})
        return data if isinstance(data, dict) else {"ok": True}

    def ble_cancel(self) -> dict[str, Any]:
        data = self._request("POST", "ble/cancel", json_body={})
        return data if isinstance(data, dict) else {"cancelled": True}

    # ------------------------------------------------------------------
    # USB keyboard capture (Neo HID mode)
    # ------------------------------------------------------------------

    def keyboard_recent(self) -> dict[str, Any]:
        data = self._request("GET", "keyboard/recent")
        return data if isinstance(data, dict) else {}

    def keyboard_raw(self) -> dict[str, Any]:
        data = self._request("GET", "keyboard/raw")
        return data if isinstance(data, dict) else {}

    def keyboard_clear(self) -> dict[str, Any]:
        data = self._request("POST", "keyboard/clear", json_body={})
        return data if isinstance(data, dict) else {"ok": True}

    # ------------------------------------------------------------------
    # Local backup files on buddy (SD / spiflash)
    # ------------------------------------------------------------------

    def list_backups(self) -> list[dict[str, Any]]:
        """List UTF-8 backup files stored on the buddy (SD or spiflash)."""
        data = self._request("GET", "files")
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected files list payload")

    def view_backup(self, name: str) -> str:
        """Read backup file contents as text (GET /files/view)."""
        return self._request_text("GET", "files/view", params={"name": name})

    def upload_backup(
        self,
        name: str,
        content: str | bytes,
        *,
        as_json: bool = False,
    ) -> dict[str, Any]:
        """Upload a file to buddy storage (POST /files)."""
        if as_json:
            body = {"name": name, "content": content.decode() if isinstance(content, bytes) else content}
            data = self._request("POST", "files", json_body=body)
        else:
            payload = content.encode("utf-8") if isinstance(content, str) else content
            data = self._request(
                "POST",
                "files",
                data=payload,
                params={"name": name},
                headers={"Content-Type": "application/octet-stream"},
            )
        return data if isinstance(data, dict) else {"ok": True}

    def rename_backup(self, old_name: str, new_name: str) -> dict[str, Any]:
        data = self._request("PATCH", "files", json_body={"old_name": old_name, "new_name": new_name})
        return data if isinstance(data, dict) else {"ok": True}

    def delete_backup(self, name: str) -> dict[str, Any]:
        data = self._request("DELETE", "files", params={"name": name, "confirm": "true"})
        return data if isinstance(data, dict) else {"deleted": True}

    def download_backup(self, name: str, dest: str | Path) -> Path:
        """Download one buddy-local backup file to the PC."""
        return self._download_to_path("files/download", dest, params={"name": name})

    def pull_backups(self, dest_dir: str | Path) -> list[Path]:
        """Download every buddy-local backup into dest_dir."""
        dest = Path(dest_dir)
        dest.mkdir(parents=True, exist_ok=True)
        saved: list[Path] = []
        for item in self.list_backups():
            name = item.get("name") if isinstance(item, dict) else None
            if not name:
                continue
            saved.append(self.download_backup(str(name), dest / str(name)))
        return saved

    # ------------------------------------------------------------------
    # Neo USB link
    # ------------------------------------------------------------------

    def rescan(self) -> dict[str, Any]:
        """Ask the buddy to scan OTG1 / connect Neo."""
        data = self._request("POST", "neo/rescan")
        return data if isinstance(data, dict) else {"ok": True}

    def restart_keyboard(self) -> dict[str, Any]:
        """Return Neo to keyboard emulation mode."""
        data = self._request("POST", "neo/restart")
        return data if isinstance(data, dict) else {"ok": True}

    def neo_debug(self, limit: int = 20) -> list[dict[str, Any]]:
        """Recent Neo USB / protocol trace events."""
        data = self._request("GET", "neo/debug", params={"limit": limit})
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected neo debug payload")

    # ------------------------------------------------------------------
    # Backups (primary API)
    # ------------------------------------------------------------------

    def autobackup_status(self) -> dict[str, Any]:
        """Progress/status for auto-backup (phase, current/total, busy)."""
        data = self._request("GET", "neo/autobackup")
        if not isinstance(data, dict):
            raise ApiError("Unexpected autobackup status")
        return data

    def backup_now(
        self,
        *,
        wait: bool = True,
        timeout: float = 600.0,
        poll: float = 1.5,
        on_progress: Callable[[dict[str, Any]], None] | None = None,
    ) -> dict[str, Any]:
        """
        Backup changed AlphaWord files, then return Neo to keyboard mode.

        This is the recommended remote backup call (matches portal Backup now).
        """
        self._request("POST", "neo/autobackup")
        if not wait:
            return {"started": True}
        return self.wait_backup(timeout=timeout, poll=poll, on_progress=on_progress)

    def backup_all(
        self,
        applet_id: int = ALPHAWORD_APPLET_ID,
        *,
        charmap: str = "en-us",
        timeout: float = 600.0,
    ) -> dict[str, Any]:
        """
        Backup every non-empty file on an applet to the buddy's SD/spiflash.

        Returns JSON with count/saved paths and returned_to_keyboard.
        """
        path = f"neo/applets/{applet_id}/files/read-all"
        data = self._request(
            "POST",
            path,
            params={"map": charmap},
            timeout=timeout,
        )
        return data if isinstance(data, dict) else {"ok": True}

    def backup_file(
        self,
        file_index: int,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
        charmap: str = "en-us",
    ) -> dict[str, Any]:
        """Read one Neo file and save it as a local backup on the buddy."""
        path = f"neo/applets/{applet_id}/files/{file_index}/read"
        data = self._request("POST", path, params={"backup": "1", "map": charmap})
        return data if isinstance(data, dict) else {"saved": True}

    def wait_backup(
        self,
        *,
        timeout: float = 600.0,
        poll: float = 1.5,
        on_progress: Callable[[dict[str, Any]], None] | None = None,
    ) -> dict[str, Any]:
        """Wait until an in-progress auto-backup finishes."""
        deadline = time.monotonic() + timeout
        saw_busy = False
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            last = self.autobackup_status()
            if on_progress:
                on_progress(last)
            busy = bool(last.get("busy"))
            if busy:
                saw_busy = True
            elif saw_busy or last.get("phase") in ("done", "idle"):
                result = last.get("last_result", "ESP_OK")
                if result and result != "ESP_OK" and saw_busy:
                    raise Neo2BuddyError(f"Backup finished with error: {result}")
                return last
            time.sleep(poll)
        raise Neo2BuddyError(f"Backup timed out after {timeout:.0f}s (last={last})")

    def backup_and_pull(
        self,
        dest_dir: str | Path,
        *,
        mode: str = "changed",
        on_progress: Callable[[dict[str, Any]], None] | None = None,
    ) -> dict[str, Any]:
        """
        Run a backup on the buddy, then download all local backups to dest_dir.

        mode:
          - "changed": auto-backup (changed files only) + return to keyboard
          - "all": backup every non-empty AlphaWord file + return to keyboard
        """
        if mode == "changed":
            result = self.backup_now(on_progress=on_progress)
        elif mode == "all":
            result = self.backup_all()
        else:
            raise ValueError("mode must be 'changed' or 'all'")
        paths = self.pull_backups(dest_dir)
        return {"backup": result, "downloaded": [str(p) for p in paths]}

    # ------------------------------------------------------------------
    # Neo documents / applets
    # ------------------------------------------------------------------

    def list_neo_files(self) -> list[dict[str, Any]]:
        """List files currently on the connected Neo (all applets)."""
        data = self._request("GET", "neo/files")
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected neo files payload")

    def list_applets(self) -> list[dict[str, Any]]:
        """Installed SmartApplets (GET /command/list_applets)."""
        data = self._request("GET", "command/list_applets")
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected applet list payload")

    def neo_info(self) -> dict[str, Any]:
        """Neo system info: version, free space, etc. (GET /command/info)."""
        data = self._request("GET", "command/info")
        if not isinstance(data, dict):
            raise ApiError("Unexpected neo info payload")
        return data

    def neo_mode(self) -> dict[str, Any]:
        """Current Neo USB mode: keyboard or comms (GET /command/mode)."""
        data = self._request("GET", "command/mode")
        if not isinstance(data, dict):
            raise ApiError("Unexpected neo mode payload")
        return data

    def neo_version(self) -> dict[str, Any]:
        data = self._request("GET", "command/version")
        return data if isinstance(data, dict) else {"version": data}

    def space_available(self) -> dict[str, Any]:
        data = self._request("GET", "command/space/available")
        if not isinstance(data, dict):
            raise ApiError("Unexpected space payload")
        return data

    def space_used(self, applet_id: int) -> dict[str, Any]:
        data = self._request("GET", "command/space/used", params={"applet_id": applet_id})
        if not isinstance(data, dict):
            raise ApiError("Unexpected space used payload")
        return data

    def file_attributes(self, index: int) -> dict[str, Any]:
        """Legacy AlphaWord file attrs by index (GET /command/file/attrs)."""
        data = self._request("GET", "command/file/attrs", params={"index": index})
        if not isinstance(data, dict):
            raise ApiError("Unexpected file attrs payload")
        return data

    def read_file_text(
        self,
        file_index: int,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
        charmap: str = "en-us",
    ) -> str:
        """Read one Neo file as UTF-8 text (does not save on buddy)."""
        path = f"neo/applets/{applet_id}/files/{file_index}/read"
        result = self._request_text("POST", path, params={"map": charmap})
        return result

    def download_neo_file(
        self,
        file_index: int,
        dest: str | Path,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
        charmap: str = "en-us",
    ) -> Path:
        """Download one Neo document as UTF-8 text."""
        path = f"neo/applets/{applet_id}/files/{file_index}/download"
        return self._download_to_path(path, dest, params={"map": charmap})

    def write_file_text(
        self,
        file_index: int,
        text: str,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
        charmap: str = "en-us",
    ) -> dict[str, Any]:
        """Import UTF-8 text into a Neo file slot."""
        path = f"neo/applets/{applet_id}/files/{file_index}/write"
        data = self._request(
            "POST",
            path,
            data=text.encode("utf-8"),
            params={"map": charmap},
            headers={"Content-Type": "text/plain; charset=utf-8"},
        )
        return data if isinstance(data, dict) else {"ok": True}

    def clear_file(
        self,
        file_index: int,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
    ) -> dict[str, Any]:
        path = f"neo/applets/{applet_id}/files/{file_index}"
        data = self._request("DELETE", path)
        return data if isinstance(data, dict) else {"ok": True}

    def write_file_by_target(
        self,
        target: str,
        text: str,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
        charmap: str = "en-us",
    ) -> dict[str, Any]:
        """Write or create a file by name or file-space number (e.g. target='3')."""
        path = f"neo/applets/{applet_id}/files/write"
        data = self._request(
            "POST",
            path,
            data=text.encode("utf-8"),
            params={"target": target, "map": charmap},
            headers={"Content-Type": "text/plain; charset=utf-8"},
        )
        return data if isinstance(data, dict) else {"ok": True}

    def clear_file_by_target(
        self,
        target: str,
        *,
        applet_id: int = ALPHAWORD_APPLET_ID,
    ) -> dict[str, Any]:
        path = f"neo/applets/{applet_id}/files"
        data = self._request("DELETE", path, params={"target": target})
        return data if isinstance(data, dict) else {"ok": True}

    def inspect_applet(self, package: bytes) -> dict[str, Any]:
        """Parse .os3kapp header locally on the buddy (no USB required)."""
        data = self._request(
            "POST",
            "neo/applets/inspect",
            data=package,
            headers={"Content-Type": "application/octet-stream"},
        )
        if not isinstance(data, dict):
            raise ApiError("Unexpected applet inspect payload")
        return data

    def install_applet(self, package: bytes, *, replace: bool = False) -> dict[str, Any]:
        """Install a SmartApplet package (.os3kapp bytes)."""
        data = self._request(
            "POST",
            "neo/applets",
            data=package,
            headers={
                "Content-Type": "application/octet-stream",
                "X-Neo-Replace": "true" if replace else "false",
            },
            timeout=max(self.timeout, 120.0),
        )
        return data if isinstance(data, dict) else {"ok": True}

    def install_applet_file(self, path: str | Path, *, replace: bool = False) -> dict[str, Any]:
        return self.install_applet(Path(path).read_bytes(), replace=replace)

    def remove_applet(self, applet_id: int) -> dict[str, Any]:
        data = self._request("DELETE", f"neo/applets/{applet_id}")
        return data if isinstance(data, dict) else {"ok": True}

    def remove_all_applets(self) -> dict[str, Any]:
        data = self._request("DELETE", "neo/applets")
        return data if isinstance(data, dict) else {"ok": True}

    def download_applet(self, applet_id: int, dest: str | Path) -> Path:
        """Download installed applet binary (.os3kapp)."""
        return self._download_to_path(f"neo/applets/{applet_id}/download", dest)

    def get_applet_settings(self, applet_id: int, flags: int = 0) -> list[dict[str, Any]]:
        data = self._request("GET", "command/settings", params={"applet_id": applet_id, "flags": flags})
        if isinstance(data, list):
            return data
        raise ApiError("Unexpected applet settings payload")

    def set_applet_settings(self, applet_id: int, items: list[dict[str, Any]]) -> dict[str, Any]:
        data = self._request(
            "POST",
            "command/settings",
            json_body={"applet_id": applet_id, "items": items},
        )
        return data if isinstance(data, dict) else {"ok": True}

    def set_applet_setting_by_ident(
        self,
        applet_id: int,
        ident: int,
        values: list[Any],
    ) -> dict[str, Any]:
        data = self._request(
            "POST",
            "command/settings/by-ident",
            json_body={"applet_id": applet_id, "ident": ident, "values": values},
        )
        return data if isinstance(data, dict) else {"ok": True}
