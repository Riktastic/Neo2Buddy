"""Tk Image Builder — build custom Neo2 Buddy firmware with ESP-IDF."""

from __future__ import annotations

import queue
import re
import sys
import threading
import webbrowser
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

from . import __version__
from .build import build_and_export, default_export_name, idf_available
from .flash import (
    flash_firmware,
    format_connection_help,
    list_serial_ports,
    parse_connection_info,
    verify_boot,
)
from .profiles import PRESET_LABELS, PRESETS, FeatureSet

C = {
    "paper": "#f3f0e8",
    "ink": "#172331",
    "muted": "#5d6b73",
    "panel": "#fffcf6",
    "cream": "#fffefa",
    "line": "#b8b0a2",
    "line_strong": "#8f8778",
    "blue": "#165d7d",
    "blue_deep": "#0c334b",
    "coral": "#d85f3f",
    "coral_hover": "#c24f32",
    "sun": "#efbb4c",
    "mint": "#b9dbcf",
    "th": "#ebe6da",
    "ok": "#2e7d4a",
}

FONT_UI = ("Segoe UI", 10)
FONT_UI_BOLD = ("Segoe UI", 10, "bold")
FONT_TITLE = ("Georgia", 24)
FONT_STEP = ("Georgia", 12)
FONT_EYEBROW = ("Segoe UI", 8, "bold")
FONT_MUTED = ("Segoe UI", 9)
FONT_BTN = ("Segoe UI", 11, "bold")
FONT_BTN_SM = ("Segoe UI", 9, "bold")
FONT_MONO = ("Consolas", 9)

GITHUB_URL = "https://github.com/Riktastic/Neo2Buddy"


class _QueueWriter:
    def __init__(self, q: queue.Queue[str]):
        self._q = q

    def write(self, text: str) -> int:
        if text:
            self._q.put(text)
        return len(text) if text else 0

    def flush(self) -> None:
        return


class BuilderApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Neo2 Buddy Image Builder")
        self.minsize(600, 520)
        self.geometry("660x760")
        self.configure(bg=C["paper"])

        self._log_q: queue.Queue[str] = queue.Queue()
        self._busy = False
        self._last_export: Path | None = None
        self._preset = tk.StringVar(value="full")
        self._export_name = tk.StringVar(value="")
        self._port = tk.StringVar()
        self._baud = tk.StringVar(value="460800")
        self._manual = tk.BooleanVar(value=True)
        self._fullclean = tk.BooleanVar(value=True)
        self._status = tk.StringVar(value="Ready")
        self._idf_label = tk.StringVar(value="Checking ESP-IDF…")
        self._show_details = False
        self._body: tk.Frame | None = None

        self._wifi = tk.BooleanVar(value=True)
        self._ble = tk.BooleanVar(value=True)
        self._store = tk.BooleanVar(value=True)
        self._oled = tk.BooleanVar(value=True)
        self._sd = tk.BooleanVar(value=True)
        self._battery = tk.BooleanVar(value=True)
        self._uart = tk.BooleanVar(value=True)

        self._setup_style()
        self._build()
        self._apply_preset("full")
        self._refresh_idf()
        self.refresh_ports()
        self.after(100, self._drain_log)
        self.after_idle(self._fit_window_height)
        self.after(80, self._fit_window_height)

    def _setup_style(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(
            "Panel.TCheckbutton",
            background=C["panel"],
            foreground=C["ink"],
            font=FONT_UI,
            focuscolor=C["panel"],
        )
        style.map(
            "Panel.TCheckbutton",
            background=[("active", C["panel"])],
            foreground=[("active", C["ink"])],
        )
        style.configure(
            "Portal.TCombobox",
            fieldbackground=C["cream"],
            background=C["panel"],
            foreground=C["ink"],
            arrowcolor=C["blue_deep"],
            bordercolor=C["line_strong"],
            lightcolor=C["line"],
            darkcolor=C["line_strong"],
            padding=5,
        )
        style.configure(
            "Portal.TEntry",
            fieldbackground=C["cream"],
            foreground=C["ink"],
            bordercolor=C["line_strong"],
            lightcolor=C["line"],
            darkcolor=C["line_strong"],
            padding=5,
        )
        style.configure(
            "Portal.Horizontal.TProgressbar",
            troughcolor=C["th"],
            background=C["coral"],
            bordercolor=C["line"],
            lightcolor=C["coral"],
            darkcolor=C["coral_hover"],
            thickness=14,
        )

    def _ghost_btn(self, parent: tk.Misc, text: str, command) -> tk.Button:
        return tk.Button(
            parent,
            text=text,
            command=command,
            font=FONT_BTN_SM,
            bg=C["panel"],
            fg=C["ink"],
            activebackground=C["mint"],
            activeforeground=C["ink"],
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground=C["line"],
            highlightcolor=C["blue"],
            bd=1,
            padx=10,
            pady=5,
            cursor="hand2",
        )

    def _card(self, parent: tk.Misc, title: str) -> tk.Frame:
        wrap = tk.Frame(parent, bg=C["paper"])
        wrap.pack(fill=tk.X, pady=(0, 12))
        shadow = tk.Frame(wrap, bg=C["line"])
        shadow.pack(fill=tk.BOTH, expand=True)
        card = tk.Frame(
            shadow,
            bg=C["panel"],
            highlightthickness=1,
            highlightbackground=C["line_strong"],
            padx=16,
            pady=12,
        )
        card.pack(fill=tk.BOTH, expand=True, padx=(0, 4), pady=(0, 4))
        tk.Label(card, text=title, bg=C["panel"], fg=C["ink"], font=FONT_STEP, anchor="w").pack(
            fill=tk.X, pady=(0, 6)
        )
        return card

    def _build(self) -> None:
        root = tk.Frame(self, bg=C["paper"], padx=22, pady=16)
        root.pack(fill=tk.BOTH, expand=True)
        self._body = root

        head = tk.Frame(root, bg=C["paper"])
        head.pack(fill=tk.X)
        tk.Label(head, text="IMAGE BUILDER", bg=C["paper"], fg=C["coral"], font=FONT_EYEBROW).pack(
            anchor="w"
        )
        title_row = tk.Frame(head, bg=C["paper"])
        title_row.pack(fill=tk.X, pady=(2, 0))
        tk.Label(title_row, text="Neo2 Buddy", bg=C["paper"], fg=C["ink"], font=FONT_TITLE).pack(
            side=tk.LEFT
        )
        chip = tk.Frame(
            title_row,
            bg=C["panel"],
            highlightthickness=1,
            highlightbackground=C["line"],
            padx=8,
            pady=4,
        )
        chip.pack(side=tk.RIGHT)
        self._status_dot = tk.Canvas(chip, width=10, height=10, bg=C["panel"], highlightthickness=0)
        self._status_dot.pack(side=tk.LEFT, padx=(0, 6))
        self._draw_status_dot(C["sun"])
        tk.Label(chip, textvariable=self._status, bg=C["panel"], fg=C["muted"], font=FONT_MUTED).pack(
            side=tk.LEFT
        )

        tk.Frame(root, bg=C["ink"], height=1).pack(fill=tk.X, pady=(10, 8))
        tk.Label(
            root,
            text="Toggle features, build with ESP-IDF, then flash — for developers and power users.",
            bg=C["paper"],
            fg=C["muted"],
            font=FONT_UI,
            wraplength=600,
            justify="left",
        ).pack(anchor="w", pady=(0, 10))

        idf_card = self._card(root, "ESP-IDF")
        self._idf_status = tk.Label(
            idf_card,
            textvariable=self._idf_label,
            bg=C["panel"],
            fg=C["ink"],
            font=FONT_MUTED,
            wraplength=560,
            justify="left",
            anchor="w",
        )
        self._idf_status.pack(fill=tk.X)
        self._ghost_btn(idf_card, "Recheck", self._refresh_idf).pack(anchor="w", pady=(8, 0))

        feat = self._card(root, "1 · Features")
        preset_row = tk.Frame(feat, bg=C["panel"])
        preset_row.pack(fill=tk.X, pady=(0, 8))
        tk.Label(preset_row, text="Preset", bg=C["panel"], fg=C["ink"], font=FONT_UI_BOLD, width=9, anchor="w").pack(
            side=tk.LEFT
        )
        self._preset_combo = ttk.Combobox(
            preset_row,
            textvariable=self._preset,
            values=list(PRESET_LABELS.keys()),
            state="readonly",
            style="Portal.TCombobox",
            width=18,
        )
        self._preset_combo.pack(side=tk.LEFT, padx=(0, 8))
        self._preset_combo.bind("<<ComboboxSelected>>", lambda _e: self._apply_preset(self._preset.get()))
        self._ghost_btn(preset_row, "Apply preset", lambda: self._apply_preset(self._preset.get())).pack(
            side=tk.LEFT
        )

        for text, var in (
            ("Wi‑Fi + web portal (off = UART-only)", self._wifi),
            ("Bluetooth HID keyboard", self._ble),
            ("Applet Store (bundled SmartApplets)", self._store),
            ("OLED status display", self._oled),
            ("microSD card", self._sd),
            ("Battery monitor", self._battery),
            ("UART command console", self._uart),
        ):
            ttk.Checkbutton(feat, text=text, variable=var, style="Panel.TCheckbutton").pack(anchor="w")

        self._wifi.trace_add("write", lambda *_: self._on_wifi_toggle())

        build_card = self._card(root, "2 · Build")
        name_row = tk.Frame(build_card, bg=C["panel"])
        name_row.pack(fill=tk.X)
        tk.Label(
            name_row, text="Export name", bg=C["panel"], fg=C["ink"], font=FONT_UI_BOLD, width=11, anchor="w"
        ).pack(side=tk.LEFT)
        ttk.Entry(name_row, textvariable=self._export_name, style="Portal.TEntry").pack(
            side=tk.LEFT, fill=tk.X, expand=True
        )
        ttk.Checkbutton(
            build_card,
            text="Full clean before build (recommended when changing features)",
            variable=self._fullclean,
            style="Panel.TCheckbutton",
        ).pack(anchor="w", pady=(8, 0))

        flash_card = self._card(root, "3 · Flash (optional)")
        port_row = tk.Frame(flash_card, bg=C["panel"])
        port_row.pack(fill=tk.X)
        tk.Label(port_row, text="USB port", bg=C["panel"], fg=C["ink"], font=FONT_UI_BOLD, width=9, anchor="w").pack(
            side=tk.LEFT
        )
        self._port_combo = ttk.Combobox(
            port_row, textvariable=self._port, state="readonly", style="Portal.TCombobox"
        )
        self._port_combo.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))
        self._ghost_btn(port_row, "Find ports", self.refresh_ports).pack(side=tk.LEFT)
        ttk.Checkbutton(
            flash_card,
            text="Board is already in download mode",
            variable=self._manual,
            style="Panel.TCheckbutton",
        ).pack(anchor="w", pady=(8, 0))

        btn_row = tk.Frame(root, bg=C["paper"])
        btn_row.pack(fill=tk.X, pady=(2, 6))
        self._build_btn = tk.Button(
            btn_row,
            text="Build image",
            command=self.start_build,
            font=FONT_BTN,
            bg=C["coral"],
            fg=C["cream"],
            activebackground=C["coral_hover"],
            activeforeground=C["cream"],
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground=C["ink"],
            bd=1,
            padx=20,
            pady=10,
            cursor="hand2",
        )
        self._build_btn.pack(side=tk.LEFT)
        self._flash_btn = self._ghost_btn(btn_row, "Flash last build", self.start_flash)
        self._flash_btn.pack(side=tk.LEFT, padx=(10, 0))
        self._details_btn = self._ghost_btn(btn_row, "Show details  ▸", self._toggle_details)
        self._details_btn.pack(side=tk.LEFT, padx=(10, 0))

        meta = tk.Frame(btn_row, bg=C["paper"])
        meta.pack(side=tk.RIGHT)
        gh = tk.Label(meta, text="GitHub", bg=C["paper"], fg=C["blue"], font=FONT_MUTED, cursor="hand2")
        gh.pack(side=tk.LEFT)
        gh.bind("<Button-1>", lambda _e: webbrowser.open(GITHUB_URL))
        tk.Label(meta, text=f" · v{__version__}", bg=C["paper"], fg=C["muted"], font=FONT_MUTED).pack(
            side=tk.LEFT
        )

        self._progress = ttk.Progressbar(
            root,
            orient="horizontal",
            mode="indeterminate",
            style="Portal.Horizontal.TProgressbar",
        )
        self._progress.pack(fill=tk.X, pady=(4, 2))

        self._log_frame = tk.Frame(root, bg=C["paper"])
        log_shadow = tk.Frame(self._log_frame, bg=C["line"])
        log_shadow.pack(fill=tk.BOTH, expand=True)
        log_inner = tk.Frame(log_shadow, bg=C["blue_deep"])
        log_inner.pack(fill=tk.BOTH, expand=True, padx=(0, 4), pady=(0, 4))
        self._log = tk.Text(
            log_inner,
            height=10,
            wrap=tk.WORD,
            font=FONT_MONO,
            bg=C["blue_deep"],
            fg="#e8f4f8",
            relief=tk.FLAT,
            padx=10,
            pady=8,
            borderwidth=0,
            highlightthickness=0,
        )
        scroll = ttk.Scrollbar(log_inner, orient="vertical", command=self._log.yview)
        self._log.configure(yscrollcommand=scroll.set)
        self._log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)

    def _draw_status_dot(self, color: str) -> None:
        self._status_dot.delete("all")
        self._status_dot.create_oval(1, 1, 9, 9, fill=color, outline=color)

    def _set_status(self, text: str, *, dot: str | None = None) -> None:
        self._status.set(text)
        if dot:
            self._draw_status_dot(dot)

    def _refresh_idf(self) -> None:
        ok, msg = idf_available()
        self._idf_label.set(msg)
        if hasattr(self, "_idf_status"):
            self._idf_status.configure(fg=C["ok"] if ok else C["coral"])
        if not ok:
            self._set_status("ESP-IDF required", dot=C["coral"])
        else:
            self._set_status("Ready to build", dot=C["ok"])

    def _features(self) -> FeatureSet:
        wifi = self._wifi.get()
        return FeatureSet(
            wifi_web=wifi,
            ble=self._ble.get(),
            stock_applets=self._store.get() and wifi,
            oled=self._oled.get(),
            sdcard=self._sd.get(),
            battery=self._battery.get(),
            uart_cmd=self._uart.get(),
        )

    def _apply_preset(self, preset_id: str) -> None:
        feats = PRESETS.get(preset_id, FeatureSet())
        self._wifi.set(feats.wifi_web)
        self._ble.set(feats.ble)
        self._store.set(feats.stock_applets)
        self._oled.set(feats.oled)
        self._sd.set(feats.sdcard)
        self._battery.set(feats.battery)
        self._uart.set(feats.uart_cmd)
        self._on_wifi_toggle()
        if not self._export_name.get().strip():
            self._export_name.set(preset_id)

    def _on_wifi_toggle(self) -> None:
        if not self._wifi.get():
            self._store.set(False)

    def _toggle_details(self) -> None:
        self._show_details = not self._show_details
        if self._show_details:
            self._log_frame.pack(fill=tk.BOTH, expand=True)
            self._details_btn.configure(text="Hide details  ▾")
        else:
            self._log_frame.pack_forget()
            self._details_btn.configure(text="Show details  ▸")
        self.after_idle(self._fit_window_height)

    def _fit_window_height(self) -> None:
        self.update_idletasks()
        body = self._body
        if body is None:
            return
        body.update_idletasks()
        needed_h = body.winfo_reqheight() + 4
        needed_w = max(body.winfo_reqwidth() + 4, 660)
        width = max(needed_w, 600)
        if self.winfo_width() > width:
            width = self.winfo_width()
        screen_h = self.winfo_screenheight()
        height = min(needed_h, max(520, screen_h - 80))
        height = max(height, 520)
        self.geometry(f"{width}x{height}")

    def _append_log(self, text: str) -> None:
        self._log.insert(tk.END, text)
        self._log.see(tk.END)

    def _drain_log(self) -> None:
        try:
            while True:
                self._append_log(self._log_q.get_nowait())
        except queue.Empty:
            pass
        self.after(80, self._drain_log)

    def refresh_ports(self) -> None:
        try:
            ports = list_serial_ports()
        except Exception as exc:
            messagebox.showerror("USB ports", f"Could not list USB ports.\n\n{exc}")
            return
        labels = [label for _, label in ports]
        self._port_combo["values"] = labels
        self._port_map = {label: device for device, label in ports}
        if labels:
            if self._port.get() not in labels:
                self._port.set(labels[0])
        else:
            self._port.set("")
            self._port_map = {}

    def _selected_port(self) -> str | None:
        label = self._port.get().strip()
        if not label:
            return None
        return getattr(self, "_port_map", {}).get(label, label.split(" —", 1)[0].strip())

    def _sanitize_name(self, name: str) -> str:
        name = name.strip().lower()
        name = re.sub(r"[^a-z0-9._-]+", "-", name)
        return name.strip("-") or default_export_name(self._preset.get(), self._features())

    def start_build(self) -> None:
        if self._busy:
            return
        ok, msg = idf_available()
        if not ok:
            messagebox.showerror("ESP-IDF required", msg + "\n\nOr use Setup with a published profile.")
            return
        features = self._features()
        if not features.wifi_web and not features.uart_cmd:
            messagebox.showwarning(
                "Nothing left",
                "UART-only builds should keep the UART console enabled so you can talk to the buddy.",
            )
            return
        name = self._sanitize_name(self._export_name.get() or default_export_name(self._preset.get(), features))
        self._export_name.set(name)
        preset_id = self._preset.get() if self._preset.get() in PRESETS else None
        # If toggles differ from the selected preset, treat as custom.
        if preset_id and PRESETS[preset_id] != features:
            preset_id = None

        self._busy = True
        self._build_btn.configure(state=tk.DISABLED, bg=C["line"], fg=C["muted"])
        self._progress.start(12)
        self._set_status("Building…", dot=C["sun"])
        if not self._show_details:
            self._toggle_details()
        self._append_log(f"\n—— Building {name} ——\n")

        def work() -> None:
            writer = _QueueWriter(self._log_q)
            try:
                out = build_and_export(
                    features,
                    export_name=name,
                    preset_id=preset_id,
                    fullclean=self._fullclean.get(),
                    log=writer.write,
                )
                self._last_export = out
                self.after(0, lambda: self._build_done(True, out))
            except Exception as exc:
                self._log_q.put(f"\nBuild failed: {exc}\n")
                self.after(0, lambda: self._build_done(False, None, str(exc)))

        threading.Thread(target=work, daemon=True).start()

    def _build_done(self, ok: bool, out: Path | None, err: str | None = None) -> None:
        self._busy = False
        self._progress.stop()
        self._build_btn.configure(state=tk.NORMAL, bg=C["coral"], fg=C["cream"])
        if ok and out:
            self._set_status("Build complete", dot=C["ok"])
            messagebox.showinfo(
                "Build complete",
                f"Images exported to:\n{out}\n\n"
                "Setup can pick this folder under Firmware profile, or click Flash last build.",
            )
        else:
            self._set_status("Build failed", dot=C["coral"])
            messagebox.showerror("Build failed", err or "See details log.")

    def start_flash(self) -> None:
        if self._busy:
            return
        images = self._last_export
        if images is None or not images.is_dir():
            messagebox.showwarning("No build yet", "Build an image first, then flash it.")
            return
        port = self._selected_port()
        if not port:
            messagebox.showwarning("Choose a USB port", "Plug in Buddy and click Find ports.")
            return
        try:
            baud = int(self._baud.get())
        except ValueError:
            baud = 460800

        self._busy = True
        self._progress.start(12)
        self._set_status("Flashing…", dot=C["sun"])
        if not self._show_details:
            self._toggle_details()

        def work() -> None:
            writer = _QueueWriter(self._log_q)
            try:
                flash_firmware(
                    port,
                    images,
                    baud=baud,
                    manual_boot=self._manual.get(),
                    wipe_littlefs=False,
                    log=writer.write,
                )
                boot = verify_boot(port, reset_first=True, log=writer.write)
                help_text = format_connection_help(parse_connection_info(boot))
                self._log_q.put("\n" + help_text + "\n")
                self.after(0, lambda: self._flash_done(True, help_text))
            except Exception as exc:
                self._log_q.put(f"\nFlash failed: {exc}\n")
                self.after(0, lambda: self._flash_done(False, str(exc)))

        threading.Thread(target=work, daemon=True).start()

    def _flash_done(self, ok: bool, detail: str) -> None:
        self._busy = False
        self._progress.stop()
        if ok:
            self._set_status("Flash complete", dot=C["ok"])
            messagebox.showinfo("You’re all set", detail)
        else:
            self._set_status("Flash failed", dot=C["coral"])
            messagebox.showerror("Flash failed", detail)


def main() -> None:
    app = BuilderApp()
    app.mainloop()


if __name__ == "__main__":
    main()
