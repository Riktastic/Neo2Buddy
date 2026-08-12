"""CLI: neo2buddy / neo2buddy-wrapper — backup | pull | status | wifi | sync | …"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .client import ALPHAWORD_APPLET_ID, Neo2BuddyClient
from .exceptions import BusyError, Neo2BuddyError


def _cards_from_pipe_text(text: str) -> list[dict[str, str]]:
    cards: list[dict[str, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or "|" not in line:
            continue
        front, back = line.split("|", 1)
        front = front.strip()[:23]
        back = back.strip()[:23]
        if front and back:
            cards.append({"front": front, "back": back})
        if len(cards) >= 16:
            break
    return cards


def _progress(st: dict) -> None:
    phase = st.get("phase") or "?"
    cur = st.get("current")
    total = st.get("total")
    saved = st.get("saved")
    skipped = st.get("skipped")
    parts = [f"phase={phase}"]
    if cur is not None and total is not None:
        parts.append(f"file {cur}/{total}")
    if saved is not None:
        parts.append(f"saved={saved}")
    if skipped is not None:
        parts.append(f"skipped={skipped}")
    print("\r" + "  ".join(parts) + "   ", end="", flush=True)


def _client_from_args(args: argparse.Namespace) -> Neo2BuddyClient:
    return Neo2BuddyClient(
        host=args.host,
        password=args.password,
        scheme=args.scheme,
        port=args.port,
        timeout=args.timeout,
    )


def _print_json(data: object, args: argparse.Namespace) -> None:
    if args.json:
        print(json.dumps(data, indent=2))
    return


def cmd_status(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    data = client.status()
    if args.json:
        print(json.dumps(data, indent=2))
    else:
        print(f"device: {data.get('device_name') or data.get('hostname') or '?'}")
        print(f"ip: {data.get('ip') or '?'}")
        print(
            f"neo: connected={data.get('usb_connected')} "
            f"keyboard={data.get('usb_keyboard_active')} "
            f"ready={data.get('usb_neo_ready')} "
            f"flipping={data.get('usb_flipping')}"
        )
        print(
            f"auto_backup: enabled={data.get('auto_backup_on_connect')} "
            f"busy={data.get('auto_backup_busy')} "
            f"phase={data.get('auto_backup_phase')}"
        )
        print(f"ble: {data.get('ble_state')}  wifi_state={data.get('wifi_state')}")
    return 0


def cmd_backup(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    mode = args.mode
    print(f"Starting backup ({mode})…", flush=True)
    try:
        if mode == "changed":
            result = client.backup_now(
                on_progress=None if args.quiet else _progress,
                timeout=args.timeout,
            )
            if not args.quiet:
                print()
        else:
            result = client.backup_all(timeout=args.timeout)
    except BusyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("Backup finished.")
        if isinstance(result, dict):
            for key in ("saved", "skipped", "count", "phase", "last_result", "returned_to_keyboard"):
                if key in result:
                    print(f"  {key}: {result[key]}")
    if args.pull:
        paths = client.pull_backups(args.pull)
        print(f"Downloaded {len(paths)} file(s) to {args.pull}")
        for p in paths:
            print(f"  {p}")
    return 0


def cmd_pull(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    dest = Path(args.dest)
    paths = client.pull_backups(dest)
    if args.json:
        print(json.dumps([str(p) for p in paths], indent=2))
    else:
        print(f"Downloaded {len(paths)} file(s) to {dest}")
        for p in paths:
            print(f"  {p}")
    return 0


def cmd_list_backups(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    files = client.list_backups()
    if args.json:
        print(json.dumps(files, indent=2))
    else:
        if not files:
            print("(no backups on buddy)")
            return 0
        for item in files:
            name = item.get("name", "?")
            size = item.get("size")
            print(f"{name}\t{size}" if size is not None else str(name))
    return 0


def cmd_list_neo(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    files = client.list_neo_files()
    if args.json:
        print(json.dumps(files, indent=2))
    else:
        for item in files:
            print(json.dumps(item) if args.verbose else item)
    return 0


def cmd_rescan(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    data = client.rescan()
    print(json.dumps(data, indent=2) if args.json else "Rescan requested.")
    return 0


def cmd_keyboard(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    data = client.restart_keyboard()
    print(json.dumps(data, indent=2) if args.json else "Neo returning to keyboard mode.")
    return 0


def cmd_settings(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.set_label is not None or args.auto_backup is not None:
        fields = {}
        if args.set_label is not None:
            fields["neo_label"] = args.set_label
        if args.auto_backup is not None:
            fields["auto_backup_on_connect"] = args.auto_backup == "on"
        data = client.set_settings(**fields)
    else:
        data = client.get_settings()
    print(json.dumps(data, indent=2))
    return 0


def cmd_logs(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    data = client.get_logs(args.limit)
    print(json.dumps(data, indent=2) if args.json else "\n".join(
        f"{e.get('ts', '?')} [{e.get('tag', '')}] {e.get('msg', e)}" for e in data
    ))
    return 0


def cmd_wifi(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.wifi_cmd == "status":
        data = client.get_wifi()
    elif args.wifi_cmd == "scan":
        data = client.scan_wifi(auth=True)
    else:
        data = client.configure_wifi(args.ssid, args.password or "", dhcp=not args.static)
    print(json.dumps(data, indent=2) if args.json else data)
    return 0


def cmd_sync(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.sync_cmd == "config":
        if args.provider is not None or args.enabled is not None or args.endpoint is not None:
            fields = {}
            if args.provider is not None:
                fields["provider"] = args.provider
            if args.enabled is not None:
                fields["enabled"] = args.enabled == "on"
            if args.endpoint is not None:
                fields["endpoint"] = args.endpoint
            if args.folder is not None:
                fields["folder"] = args.folder
            if args.bucket is not None:
                fields["bucket"] = args.bucket
            if args.region is not None:
                fields["region"] = args.region
            if args.username is not None:
                fields["username"] = args.username
            if args.secret is not None:
                fields["secret"] = args.secret
            data = client.set_sync_config(**fields)
        else:
            data = client.get_sync_config()
    elif args.sync_cmd == "test":
        data = client.sync_test()
    else:
        data = client.sync_run(wait=not args.no_wait)
    print(json.dumps(data, indent=2) if args.json else data)
    return 0


def cmd_ble(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.ble_cmd == "status":
        data = client.get_ble()
    elif args.ble_cmd == "pair":
        data = client.ble_pairing(args.on == "on")
    elif args.ble_cmd == "preview":
        data = client.ble_preview(args.text)
    elif args.ble_cmd == "send":
        data = client.ble_send()
    else:
        data = client.ble_cancel()
    print(json.dumps(data, indent=2) if args.json else data)
    return 0


def cmd_neo(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.neo_cmd == "info":
        data = client.neo_info()
    elif args.neo_cmd == "mode":
        data = client.neo_mode()
    elif args.neo_cmd == "applets":
        data = client.list_applets()
    elif args.neo_cmd == "debug":
        data = client.neo_debug(args.limit)
    elif args.neo_cmd == "read":
        text = client.read_file_text(args.index, applet_id=args.applet_id, charmap=args.map)
        if args.json:
            print(json.dumps({"text": text}))
        else:
            print(text)
        return 0
    elif args.neo_cmd == "write":
        text = Path(args.file).read_text(encoding="utf-8")
        data = client.write_file_text(
            args.index, text, applet_id=args.applet_id, charmap=args.map
        )
    elif args.neo_cmd == "clear":
        data = client.clear_file(args.index, applet_id=args.applet_id)
    elif args.neo_cmd == "install":
        data = client.install_applet_file(args.file, replace=args.replace)
    elif args.neo_cmd == "remove":
        data = client.remove_applet(args.applet_id)
    else:
        data = client.remove_all_applets()
    print(json.dumps(data, indent=2) if args.json else data)
    return 0


def cmd_store(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.store_cmd == "install":
        data = client.install_stock_applet(args.slug)
        print(json.dumps(data, indent=2) if args.json else f"Installed {args.slug}.")
        return 0

    catalog = client.list_stock_applets()
    if args.json:
        print(json.dumps(catalog, indent=2))
        return 0
    applets = catalog.get("applets") if isinstance(catalog, dict) else None
    if not isinstance(applets, list):
        print(catalog)
        return 0
    for app in applets:
        ver = f"{app.get('version_major', '?')}.{app.get('version_minor', '?')}{app.get('version_rev') or ''}"
        aid = app.get("applet_id")
        aid_s = f"0x{int(aid):04X}" if isinstance(aid, int) else "?"
        bundled = "bundled" if app.get("bundled") else "missing"
        print(
            f"{app.get('slug'):12}  {app.get('name'):14}  {aid_s}  v{ver}  "
            f"{app.get('category')}  {bundled}  {app.get('bytes', 0)} B"
        )
    print(f"\n{catalog.get('bundled_count', 0)} bundled in firmware")
    return 0


def cmd_decks(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.decks_cmd == "list":
        decks = client.list_flash_decks()
        if args.json:
            print(json.dumps(decks, indent=2))
            return 0
        for d in decks:
            print(f"{d.get('id'):16}  {d.get('name'):24}  {d.get('cards')} cards")
        return 0

    if args.decks_cmd == "get":
        deck = client.get_flash_deck(args.id)
        print(json.dumps(deck, indent=2))
        return 0

    if args.decks_cmd == "save":
        raw = Path(args.file).read_text(encoding="utf-8")
        if args.file.lower().endswith(".json"):
            payload = json.loads(raw)
            name = args.name or payload.get("name") or args.id
            cards = payload.get("cards") or []
        else:
            name = args.name or args.id
            cards = _cards_from_pipe_text(raw)
        if not cards:
            print("error: no cards found (use JSON or front|back lines)", file=sys.stderr)
            return 1
        data = client.save_flash_deck(args.id, name=name, cards=cards)
        print(json.dumps(data, indent=2) if args.json else f"Saved {args.id} ({len(cards)} cards).")
        return 0

    if args.decks_cmd == "push":
        data = client.push_flash_deck(args.id)
        print(json.dumps(data, indent=2) if args.json else f"Pushed {args.id} to Neo.")
        return 0

    if args.decks_cmd == "delete":
        data = client.delete_flash_deck(args.id)
        print(json.dumps(data, indent=2) if args.json else f"Deleted {args.id}.")
        return 0

    # upload: direct front|back to Neo (legacy path)
    text = Path(args.file).read_text(encoding="utf-8")
    data = client.upload_flash_deck_text(text)
    print(json.dumps(data, indent=2) if args.json else "Uploaded deck to Neo Flash Cards.")
    return 0


def cmd_files(client: Neo2BuddyClient, args: argparse.Namespace) -> int:
    if args.files_cmd == "view":
        text = client.view_backup(args.name)
        print(text)
    elif args.files_cmd == "upload":
        content = Path(args.file).read_bytes()
        data = client.upload_backup(args.name, content)
        print(json.dumps(data, indent=2) if args.json else "Uploaded.")
    elif args.files_cmd == "delete":
        data = client.delete_backup(args.name)
        print(json.dumps(data, indent=2) if args.json else "Deleted.")
    else:
        path = client.download_backup(args.name, args.dest or args.name)
        print(json.dumps({"path": str(path)}, indent=2) if args.json else str(path))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="neo2buddy",
        description="Remote access to AlphaSmart Neo2 Buddy (portal API over Wi-Fi/AP).",
    )
    p.add_argument("--host", default="192.168.4.1", help="Buddy IP or URL (default: AP 192.168.4.1)")
    p.add_argument("--password", default="neo2buddy", help="Portal password")
    p.add_argument("--scheme", default="http", choices=("http", "https"))
    p.add_argument("--port", type=int, default=None)
    p.add_argument("--timeout", type=float, default=120.0, help="HTTP timeout (seconds)")
    p.add_argument("--json", action="store_true", help="Machine-readable output")
    p.add_argument("-q", "--quiet", action="store_true")

    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("status", help="Device / Neo / auto-backup status")
    s.set_defaults(func=cmd_status)

    b = sub.add_parser("backup", help="Run a backup on the buddy")
    b.add_argument("--mode", choices=("changed", "all"), default="changed")
    b.add_argument("--pull", metavar="DIR", help="After backup, download backups into DIR")
    b.set_defaults(func=cmd_backup)

    pl = sub.add_parser("pull", help="Download all buddy-local backups")
    pl.add_argument("dest", help="Destination directory")
    pl.set_defaults(func=cmd_pull)

    lb = sub.add_parser("list-backups", help="List backup files on the buddy")
    lb.set_defaults(func=cmd_list_backups)

    ln = sub.add_parser("list-neo", help="List files on the connected Neo")
    ln.add_argument("-v", "--verbose", action="store_true")
    ln.set_defaults(func=cmd_list_neo)

    r = sub.add_parser("rescan", help="Rescan USB / connect Neo")
    r.set_defaults(func=cmd_rescan)

    k = sub.add_parser("keyboard", help="Return Neo to keyboard mode")
    k.set_defaults(func=cmd_keyboard)

    st = sub.add_parser("settings", help="Get or update device settings")
    st.add_argument("--set-label", metavar="LABEL")
    st.add_argument("--auto-backup", choices=("on", "off"))
    st.set_defaults(func=cmd_settings)

    lg = sub.add_parser("logs", help="Recent portal log lines")
    lg.add_argument("--limit", type=int, default=50)
    lg.set_defaults(func=cmd_logs)

    wf = sub.add_parser("wifi", help="Wi-Fi status, scan, or connect")
    wf_sub = wf.add_subparsers(dest="wifi_cmd", required=True)
    wf_sub.add_parser("status", help="Current Wi-Fi state").set_defaults(func=cmd_wifi)
    wf_sub.add_parser("scan", help="Scan for networks").set_defaults(func=cmd_wifi)
    wfc = wf_sub.add_parser("connect", help="Connect to a network")
    wfc.add_argument("ssid")
    wfc.add_argument("password", nargs="?", default="")
    wfc.add_argument("--static", action="store_true", help="Use static IP from settings")
    wfc.set_defaults(func=cmd_wifi)

    sy = sub.add_parser("sync", help="Cloud backup sync (WebDAV / S3)")
    sy_sub = sy.add_subparsers(dest="sync_cmd", required=True)
    syc = sy_sub.add_parser("config", help="Get or update sync config")
    syc.add_argument("--provider", choices=("none", "webdav", "s3"))
    syc.add_argument("--enabled", choices=("on", "off"))
    syc.add_argument("--endpoint")
    syc.add_argument("--folder")
    syc.add_argument("--bucket")
    syc.add_argument("--region")
    syc.add_argument("--username")
    syc.add_argument("--secret", help="Password or secret key (write-only)")
    syc.set_defaults(func=cmd_sync)
    sy_sub.add_parser("test", help="Test cloud connection").set_defaults(func=cmd_sync)
    syr = sy_sub.add_parser("run", help="Upload local backups to cloud")
    syr.add_argument("--no-wait", action="store_true", help="Start upload and return immediately")
    syr.set_defaults(func=cmd_sync)

    bl = sub.add_parser("ble", help="BLE HID keyboard relay")
    bl_sub = bl.add_subparsers(dest="ble_cmd", required=True)
    bl_sub.add_parser("status").set_defaults(func=cmd_ble)
    blp = bl_sub.add_parser("pair")
    blp.add_argument("on", choices=("on", "off"))
    blp.set_defaults(func=cmd_ble)
    blpv = bl_sub.add_parser("preview")
    blpv.add_argument("text")
    blpv.set_defaults(func=cmd_ble)
    bl_sub.add_parser("send").set_defaults(func=cmd_ble)
    bl_sub.add_parser("cancel").set_defaults(func=cmd_ble)

    neo = sub.add_parser("neo", help="Neo protocol: info, files, applets, debug")
    neo_sub = neo.add_subparsers(dest="neo_cmd", required=True)
    neo_sub.add_parser("info", help="Neo system info").set_defaults(func=cmd_neo)
    neo_sub.add_parser("mode", help="keyboard vs comms mode").set_defaults(func=cmd_neo)
    neo_sub.add_parser("applets", help="List installed applets").set_defaults(func=cmd_neo)
    nd = neo_sub.add_parser("debug", help="Protocol trace")
    nd.add_argument("--limit", type=int, default=20)
    nd.set_defaults(func=cmd_neo)
    nr = neo_sub.add_parser("read", help="Read Neo file as UTF-8")
    nr.add_argument("index", type=int)
    nr.add_argument("--applet-id", type=lambda x: int(x, 0), default=ALPHAWORD_APPLET_ID)
    nr.add_argument("--map", default="en-us")
    nr.set_defaults(func=cmd_neo)
    nw = neo_sub.add_parser("write", help="Write UTF-8 file to Neo slot")
    nw.add_argument("index", type=int)
    nw.add_argument("file", help="Local UTF-8 text file")
    nw.add_argument("--applet-id", type=lambda x: int(x, 0), default=ALPHAWORD_APPLET_ID)
    nw.add_argument("--map", default="en-us")
    nw.set_defaults(func=cmd_neo)
    nc = neo_sub.add_parser("clear", help="Clear Neo file slot")
    nc.add_argument("index", type=int)
    nc.add_argument("--applet-id", type=lambda x: int(x, 0), default=ALPHAWORD_APPLET_ID)
    nc.set_defaults(func=cmd_neo)
    ni = neo_sub.add_parser("install", help="Install .os3kapp package")
    ni.add_argument("file")
    ni.add_argument("--replace", action="store_true")
    ni.set_defaults(func=cmd_neo)
    nrm = neo_sub.add_parser("remove", help="Remove one applet")
    nrm.add_argument("applet_id", type=lambda x: int(x, 0))
    nrm.set_defaults(func=cmd_neo)
    neo_sub.add_parser("remove-all", help="Remove all applets").set_defaults(func=cmd_neo)

    st_store = sub.add_parser("store", help="Stock App Store (bundled SmartApplets)")
    store_sub = st_store.add_subparsers(dest="store_cmd", required=True)
    store_sub.add_parser("list", help="List bundled stock applets").set_defaults(func=cmd_store)
    sti = store_sub.add_parser("install", help="Install a stock applet by slug")
    sti.add_argument("slug", help="e.g. flash-cards, snake, touch-type")
    sti.set_defaults(func=cmd_store)

    dk = sub.add_parser("decks", help="Flash Cards deck library")
    dk_sub = dk.add_subparsers(dest="decks_cmd", required=True)
    dk_sub.add_parser("list", help="List named decks on the buddy").set_defaults(func=cmd_decks)
    dkg = dk_sub.add_parser("get", help="Show one deck (JSON)")
    dkg.add_argument("id")
    dkg.set_defaults(func=cmd_decks)
    dks = dk_sub.add_parser("save", help="Create/update a deck from JSON or front|back text")
    dks.add_argument("id", help="Deck id (e.g. en-nl-basic)")
    dks.add_argument("file", help=".json with {name,cards} or front|back lines")
    dks.add_argument("--name", help="Display name (defaults to id / JSON name)")
    dks.set_defaults(func=cmd_decks)
    dkp = dk_sub.add_parser("push", help="Push a buddy deck to Neo Flash Cards")
    dkp.add_argument("id")
    dkp.set_defaults(func=cmd_decks)
    dkd = dk_sub.add_parser("delete", help="Delete a deck (not en-nl-basic)")
    dkd.add_argument("id")
    dkd.set_defaults(func=cmd_decks)
    dku = dk_sub.add_parser("upload", help="Upload front|back text straight to Neo (no library save)")
    dku.add_argument("file")
    dku.set_defaults(func=cmd_decks)

    fl = sub.add_parser("files", help="Buddy-local backup file management")
    fl_sub = fl.add_subparsers(dest="files_cmd", required=True)
    flv = fl_sub.add_parser("view", help="View backup contents")
    flv.add_argument("name")
    flv.set_defaults(func=cmd_files)
    flu = fl_sub.add_parser("upload", help="Upload a file to buddy storage")
    flu.add_argument("name")
    flu.add_argument("file")
    flu.set_defaults(func=cmd_files)
    fld = fl_sub.add_parser("delete", help="Delete a backup file")
    fld.add_argument("name")
    fld.set_defaults(func=cmd_files)
    flg = fl_sub.add_parser("get", help="Download one backup")
    flg.add_argument("name")
    flg.add_argument("dest", nargs="?", default=None)
    flg.set_defaults(func=cmd_files)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    client = _client_from_args(args)
    try:
        client.ensure_login()
        return int(args.func(client, args))
    except Neo2BuddyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
