"""Friendly Tkinter GUI for installing Neo2 Buddy firmware.

Visual language matches the portal (paper / ink / coral / blue).
"""

from __future__ import annotations

import queue
import sys
import threading
import webbrowser
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from . import __version__
from .flash import (
    FlashProgressTracker,
    find_images_dir,
    flash_firmware,
    format_connection_help,
    image_sizes,
    images_complete,
    list_serial_ports,
    missing_images,
    parse_connection_info,
    verify_boot,
)
from .profiles import ImageProfile, discover_image_profiles

# Match firmware-web/css/portal.css :root tokens
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

DOWNLOAD_MODE = (
    "Before installing, put the board into download mode:\n"
    "\n"
    "  1. Hold BOOT  (BUT1 on Olimex ESP32-S3 boards)\n"
    "  2. Press and release RESET  (RST1)\n"
    "  3. Let go of BOOT\n"
    "\n"
    "Then click Install within a few seconds."
)


class _QueueWriter:
    """Capture stdout/stderr into the log queue and optional progress tracker."""

    def __init__(
        self,
        q: queue.Queue[str],
        progress_q: queue.Queue[tuple[float, str]] | None = None,
        tracker: FlashProgressTracker | None = None,
    ):
        self._q = q
        self._progress_q = progress_q
        self._tracker = tracker

    def write(self, text: str) -> int:
        if text:
            self._q.put(text)
            if self._tracker is not None and self._progress_q is not None:
                for update in self._tracker.feed(text):
                    self._progress_q.put(update)
        return len(text) if text else 0

    def flush(self) -> None:
        return


class FlasherApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Neo2 Buddy Setup")
        self.minsize(560, 420)
        self.geometry("620x700")
        self.configure(bg=C["paper"])

        self._log_q: queue.Queue[str] = queue.Queue()
        self._progress_q: queue.Queue[tuple[float, str]] = queue.Queue()
        self._busy = False
        self._images = tk.StringVar()
        self._port = tk.StringVar()
        self._baud = tk.StringVar(value="460800")
        self._manual = tk.BooleanVar(value=True)
        self._verify = tk.BooleanVar(value=True)
        self._wipe_config = tk.BooleanVar(value=False)
        self._status = tk.StringVar(value="Ready when you are")
        self._progress_text = tk.StringVar(value="Waiting to install")
        self._firmware_label = tk.StringVar(value="Looking for firmware…")
        self._show_details = False
        self._body: tk.Frame | None = None
        self._progress_value = 0.0
        self._profiles: list[ImageProfile] = []
        self._profile_label = tk.StringVar()
        self._uart_only_profile = False
        self._adv_dialog: tk.Toplevel | None = None

        found = find_images_dir()
        if found:
            self._images.set(str(found))
        self._images.trace_add("write", lambda *_: self._refresh_firmware_label())

        self._setup_style()
        self._build()
        self.refresh_ports()
        self._refresh_profiles()
        self._refresh_firmware_label()
        self.after(100, self._drain_log)
        # Two passes: idle after pack, then after fonts/metrics settle on Windows.
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
            "Adv.TCheckbutton",
            background=C["th"],
            foreground=C["ink"],
            font=FONT_UI,
            focuscolor=C["th"],
        )
        style.map(
            "Adv.TCheckbutton",
            background=[("active", C["th"])],
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
        style.map(
            "Portal.TCombobox",
            fieldbackground=[("readonly", C["cream"])],
            selectbackground=[("readonly", C["mint"])],
            selectforeground=[("readonly", C["ink"])],
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
        tk.Label(head, text="FIRMWARE SETUP", bg=C["paper"], fg=C["coral"], font=FONT_EYEBROW).pack(
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
            text="Install firmware on your Buddy — no extra tools needed.",
            bg=C["paper"],
            fg=C["muted"],
            font=FONT_UI,
            wraplength=560,
            justify="left",
        ).pack(anchor="w", pady=(0, 10))

        # 1 · Connect
        step1 = self._card(root, "1 · Connect your Buddy")
        tk.Label(
            step1,
            text="Plug in the programming USB port (UART / CH340 — not the Neo keyboard cable).",
            bg=C["panel"],
            fg=C["muted"],
            font=FONT_MUTED,
            wraplength=540,
            justify="left",
        ).pack(anchor="w", pady=(0, 8))
        port_row = tk.Frame(step1, bg=C["panel"])
        port_row.pack(fill=tk.X)
        tk.Label(
            port_row, text="USB port", bg=C["panel"], fg=C["ink"], font=FONT_UI_BOLD, width=9, anchor="w"
        ).pack(side=tk.LEFT)
        self._port_combo = ttk.Combobox(
            port_row, textvariable=self._port, state="readonly", style="Portal.TCombobox"
        )
        self._port_combo.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))
        self._ghost_btn(port_row, "Find ports", self.refresh_ports).pack(side=tk.LEFT)

        dl = tk.Frame(
            step1,
            bg=C["mint"],
            highlightthickness=1,
            highlightbackground=C["line"],
            padx=12,
            pady=10,
        )
        dl.pack(fill=tk.X, pady=(12, 0))
        tk.Label(
            dl,
            text="Set download mode",
            bg=C["mint"],
            fg=C["ink"],
            font=FONT_UI_BOLD,
            anchor="w",
        ).pack(fill=tk.X)
        tk.Label(
            dl,
            text=DOWNLOAD_MODE,
            bg=C["mint"],
            fg=C["ink"],
            font=FONT_MUTED,
            wraplength=520,
            justify="left",
            anchor="w",
        ).pack(fill=tk.X, pady=(4, 0))

        # 2 · Install
        step2 = self._card(root, "2 · Install")
        profile_row = tk.Frame(step2, bg=C["panel"])
        profile_row.pack(fill=tk.X, pady=(0, 8))
        tk.Label(
            profile_row,
            text="Firmware profile",
            bg=C["panel"],
            fg=C["ink"],
            font=FONT_UI_BOLD,
            width=14,
            anchor="w",
        ).pack(side=tk.LEFT)
        self._profile_combo = ttk.Combobox(
            profile_row,
            textvariable=self._profile_label,
            state="readonly",
            style="Portal.TCombobox",
        )
        self._profile_combo.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))
        self._profile_combo.bind("<<ComboboxSelected>>", lambda _e: self._on_profile_selected())
        self._ghost_btn(profile_row, "Refresh", self._refresh_profiles).pack(side=tk.LEFT)
        tk.Label(
            step2,
            text="Full is the stock portal build. Headless and UART slim are optional "
            "prebuilt packs (or custom builds from Image Builder).",
            bg=C["panel"],
            fg=C["muted"],
            font=FONT_MUTED,
            wraplength=540,
            justify="left",
        ).pack(anchor="w", pady=(0, 8))
        ttk.Checkbutton(
            step2,
            text="Check that Buddy started after install (recommended)",
            variable=self._verify,
            style="Panel.TCheckbutton",
        ).pack(anchor="w")
        self._fw_ready = tk.Label(
            step2,
            textvariable=self._firmware_label,
            bg=C["panel"],
            fg=C["ok"],
            font=FONT_MUTED,
            anchor="w",
        )
        self._fw_ready.pack(fill=tk.X, pady=(6, 0))

        link_row = tk.Frame(step2, bg=C["panel"])
        link_row.pack(fill=tk.X, pady=(10, 0))
        self._adv_btn = self._ghost_btn(link_row, "Advanced…", self._open_advanced)
        self._adv_btn.pack(side=tk.LEFT)

        # CTA
        btn_row = tk.Frame(root, bg=C["paper"])
        btn_row.pack(fill=tk.X, pady=(2, 6))
        self._flash_btn = tk.Button(
            btn_row,
            text="Install firmware",
            command=self.start_flash,
            font=FONT_BTN,
            bg=C["coral"],
            fg=C["cream"],
            activebackground=C["coral_hover"],
            activeforeground=C["cream"],
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground=C["ink"],
            highlightcolor=C["ink"],
            bd=1,
            padx=20,
            pady=10,
            cursor="hand2",
        )
        self._flash_btn.pack(side=tk.LEFT)
        self._details_btn = self._ghost_btn(btn_row, "Show details  ▸", self._toggle_details)
        self._details_btn.pack(side=tk.LEFT, padx=(10, 0))

        meta = tk.Frame(btn_row, bg=C["paper"])
        meta.pack(side=tk.RIGHT)
        gh = tk.Label(
            meta,
            text="GitHub",
            bg=C["paper"],
            fg=C["blue"],
            font=FONT_MUTED,
            cursor="hand2",
        )
        gh.pack(side=tk.LEFT)
        gh.bind("<Button-1>", lambda _e: webbrowser.open(GITHUB_URL))
        gh.bind("<Enter>", lambda _e: gh.configure(font=("Segoe UI", 9, "underline")))
        gh.bind("<Leave>", lambda _e: gh.configure(font=FONT_MUTED))
        tk.Label(meta, text=" · ", bg=C["paper"], fg=C["muted"], font=FONT_MUTED).pack(side=tk.LEFT)
        tk.Label(meta, text=f"v{__version__}", bg=C["paper"], fg=C["muted"], font=FONT_MUTED).pack(
            side=tk.LEFT
        )

        # Progress (always visible — details log stays optional)
        prog_wrap = tk.Frame(root, bg=C["paper"])
        prog_wrap.pack(fill=tk.X, pady=(4, 2))
        prog_head = tk.Frame(prog_wrap, bg=C["paper"])
        prog_head.pack(fill=tk.X)
        tk.Label(
            prog_head,
            textvariable=self._progress_text,
            bg=C["paper"],
            fg=C["ink"],
            font=FONT_UI,
            anchor="w",
        ).pack(side=tk.LEFT)
        self._progress_pct = tk.StringVar(value="")
        tk.Label(
            prog_head,
            textvariable=self._progress_pct,
            bg=C["paper"],
            fg=C["muted"],
            font=FONT_MUTED,
            anchor="e",
        ).pack(side=tk.RIGHT)
        self._progress = ttk.Progressbar(
            prog_wrap,
            orient="horizontal",
            mode="determinate",
            maximum=1000,
            value=0,
            style="Portal.Horizontal.TProgressbar",
        )
        self._progress.pack(fill=tk.X, pady=(6, 0), ipady=1)

        self._log_frame = tk.Frame(root, bg=C["paper"])
        log_shadow = tk.Frame(self._log_frame, bg=C["line"])
        log_shadow.pack(fill=tk.BOTH, expand=True)
        log_inner = tk.Frame(log_shadow, bg=C["blue_deep"])
        log_inner.pack(fill=tk.BOTH, expand=True, padx=(0, 4), pady=(0, 4))
        self._log = tk.Text(
            log_inner,
            height=7,
            wrap=tk.WORD,
            font=FONT_MONO,
            bg=C["blue_deep"],
            fg="#e8f4f8",
            insertbackground="#e8f4f8",
            relief=tk.FLAT,
            padx=10,
            pady=8,
            borderwidth=0,
            highlightthickness=0,
        )
        log_scroll = ttk.Scrollbar(log_inner, orient="vertical", command=self._log.yview)
        self._log.configure(yscrollcommand=log_scroll.set)
        self._log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        log_scroll.pack(side=tk.RIGHT, fill=tk.Y)

    def _draw_status_dot(self, color: str) -> None:
        self._status_dot.delete("all")
        self._status_dot.create_oval(1, 1, 9, 9, fill=color, outline=color)

    def _set_status(self, text: str, *, dot: str | None = None) -> None:
        self._status.set(text)
        if dot:
            self._draw_status_dot(dot)

    def _close_advanced(self) -> None:
        dlg = self._adv_dialog
        self._adv_dialog = None
        if dlg is not None:
            try:
                dlg.grab_release()
            except tk.TclError:
                pass
            dlg.destroy()

    def _open_advanced(self) -> None:
        if self._adv_dialog is not None and self._adv_dialog.winfo_exists():
            self._adv_dialog.lift()
            self._adv_dialog.focus_force()
            return

        dlg = tk.Toplevel(self)
        self._adv_dialog = dlg
        dlg.title("Advanced")
        dlg.configure(bg=C["paper"])
        dlg.transient(self)
        dlg.resizable(False, False)
        dlg.protocol("WM_DELETE_WINDOW", self._close_advanced)

        shell = tk.Frame(dlg, bg=C["paper"], padx=18, pady=14)
        shell.pack(fill=tk.BOTH, expand=True)

        tk.Label(shell, text="ADVANCED", bg=C["paper"], fg=C["coral"], font=FONT_EYEBROW).pack(
            anchor="w"
        )
        tk.Label(
            shell,
            text="Install options",
            bg=C["paper"],
            fg=C["ink"],
            font=FONT_STEP,
            anchor="w",
        ).pack(fill=tk.X, pady=(2, 10))

        card = tk.Frame(
            shell,
            bg=C["th"],
            highlightthickness=1,
            highlightbackground=C["line"],
            padx=14,
            pady=12,
        )
        card.pack(fill=tk.BOTH, expand=True)

        tk.Label(card, text="Firmware folder", bg=C["th"], fg=C["ink"], font=FONT_UI_BOLD, anchor="w").pack(
            fill=tk.X
        )
        tk.Label(
            card,
            text="Pre-selected when you run this from the project. Change only for a different release.",
            bg=C["th"],
            fg=C["muted"],
            font=FONT_MUTED,
            wraplength=460,
            justify="left",
        ).pack(anchor="w", pady=(2, 6))
        fw_row = tk.Frame(card, bg=C["th"])
        fw_row.pack(fill=tk.X)
        ttk.Entry(fw_row, textvariable=self._images, style="Portal.TEntry").pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8)
        )
        self._ghost_btn(fw_row, "Choose…", self.browse_images).pack(side=tk.LEFT)

        tk.Frame(card, bg=C["line"], height=1).pack(fill=tk.X, pady=10)

        ttk.Checkbutton(
            card,
            text="Board is already in download mode (recommended)",
            variable=self._manual,
            style="Adv.TCheckbutton",
        ).pack(anchor="w")
        tk.Label(
            card,
            text="Leave this on after you follow the download-mode steps above. "
            "Turn it off only if your board resets into install mode by itself.",
            bg=C["th"],
            fg=C["muted"],
            font=FONT_MUTED,
            wraplength=460,
            justify="left",
        ).pack(anchor="w", pady=(2, 8))

        ttk.Checkbutton(
            card,
            text="Wipe settings & portal storage first",
            variable=self._wipe_config,
            style="Adv.TCheckbutton",
        ).pack(anchor="w")
        tk.Label(
            card,
            text="Erases Wi‑Fi, portal password, device settings, cloud credentials "
            "(NVS), and the portal file system before writing. Use this for a clean "
            "first-run setup. A fresh portal image is written afterward.",
            bg=C["th"],
            fg=C["muted"],
            font=FONT_MUTED,
            wraplength=460,
            justify="left",
        ).pack(anchor="w", pady=(2, 8))

        baud_row = tk.Frame(card, bg=C["th"])
        baud_row.pack(fill=tk.X)
        tk.Label(baud_row, text="Install speed", bg=C["th"], fg=C["muted"], font=FONT_MUTED).pack(
            side=tk.LEFT
        )
        ttk.Combobox(
            baud_row,
            textvariable=self._baud,
            values=("115200", "230400", "460800", "921600"),
            width=10,
            state="readonly",
            style="Portal.TCombobox",
        ).pack(side=tk.LEFT, padx=(8, 0))
        tk.Label(
            baud_row,
            text="Try 115200 if installs keep failing.",
            bg=C["th"],
            fg=C["muted"],
            font=FONT_MUTED,
        ).pack(side=tk.LEFT, padx=(10, 0))

        actions = tk.Frame(shell, bg=C["paper"])
        actions.pack(fill=tk.X, pady=(14, 0))
        done = tk.Button(
            actions,
            text="Done",
            command=self._close_advanced,
            font=FONT_BTN_SM,
            bg=C["coral"],
            fg=C["cream"],
            activebackground=C["coral_hover"],
            activeforeground=C["cream"],
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground=C["ink"],
            bd=1,
            padx=16,
            pady=6,
            cursor="hand2",
        )
        done.pack(side=tk.RIGHT)

        dlg.update_idletasks()
        w, h = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        px = self.winfo_rootx() + max(24, (self.winfo_width() - w) // 2)
        py = self.winfo_rooty() + max(24, (self.winfo_height() - h) // 3)
        dlg.geometry(f"+{px}+{py}")
        dlg.minsize(520, 360)
        try:
            dlg.grab_set()
        except tk.TclError:
            pass
        done.focus_set()

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
        """Grow or shrink the window so the current content fits without scrolling."""
        self.update_idletasks()
        body = self._body
        if body is None:
            return
        body.update_idletasks()

        # On Windows/macOS Tk, geometry WxH is the client area — match body req size.
        needed_h = body.winfo_reqheight() + 4
        needed_w = max(body.winfo_reqwidth() + 4, 620)

        width = max(needed_w, 560)
        if self.winfo_width() > width:
            width = self.winfo_width()

        screen_h = self.winfo_screenheight()
        height = min(needed_h, max(420, screen_h - 80))
        height = max(height, 420)
        self.geometry(f"{width}x{height}")
        self.minsize(560, 420 if not self._show_details else 520)

    def _refresh_firmware_label(self) -> None:
        path_str = self._images.get().strip()
        if not path_str:
            self._firmware_label.set("Firmware · not selected (open Advanced)")
            self._fw_ready.configure(fg=C["muted"])
            return
        path = Path(path_str)
        if images_complete(path):
            self._firmware_label.set(f"Firmware · {path.name} ready")
            self._fw_ready.configure(fg=C["ok"])
        else:
            missing = missing_images(path) if path.is_dir() else [
                "bootloader.bin",
                "partition-table.bin",
                "alpha_smart_neo2_buddy.bin",
                "littlefs.bin",
            ]
            self._firmware_label.set(f"Firmware · incomplete ({len(missing)} missing) — open Advanced")
            self._fw_ready.configure(fg=C["coral"])

    def _refresh_profiles(self) -> None:
        self._profiles = discover_image_profiles()
        labels = [p.label for p in self._profiles]
        self._profile_combo["values"] = labels
        if not labels:
            self._profile_label.set("")
            self._uart_only_profile = False
            return
        current = self._images.get().strip()
        match = None
        if current:
            cur = Path(current).resolve()
            for p in self._profiles:
                if p.path.resolve() == cur:
                    match = p
                    break
        if match is None:
            # Prefer Full / bundled over custom.
            for p in self._profiles:
                if p.id in ("full", "bundled"):
                    match = p
                    break
            if match is None:
                match = self._profiles[0]
        self._profile_label.set(match.label)
        self._images.set(str(match.path))
        self._uart_only_profile = match.uart_only
        self._refresh_firmware_label()

    def _on_profile_selected(self) -> None:
        label = self._profile_label.get()
        for p in self._profiles:
            if p.label == label:
                self._images.set(str(p.path))
                self._uart_only_profile = p.uart_only
                self._refresh_firmware_label()
                return

    def browse_images(self) -> None:
        path = filedialog.askdirectory(title="Choose the firmware folder")
        if path:
            self._images.set(path)
            self._uart_only_profile = "uart" in Path(path).name.lower()
            # Keep profile combo in sync when possible.
            for p in self._profiles:
                if p.path.resolve() == Path(path).resolve():
                    self._profile_label.set(p.label)
                    self._uart_only_profile = p.uart_only
                    break
            else:
                self._profile_label.set(f"Custom folder · {Path(path).name}")

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
            current = self._port.get()
            if current not in labels:
                self._port.set(labels[0])
            self._set_status(f"Found {len(labels)} USB port(s)", dot=C["ok"])
        else:
            self._port.set("")
            self._port_map = {}
            self._set_status("No USB port — plug in Buddy, then Find ports", dot=C["sun"])
            self._append_log(
                "No USB ports found. Plug in the Buddy’s programming USB cable, then click Find ports.\n"
            )

    def _selected_port(self) -> str | None:
        label = self._port.get().strip()
        if not label:
            return None
        return getattr(self, "_port_map", {}).get(label, label.split(" —", 1)[0].strip())

    def _set_progress(self, fraction: float, text: str | None = None) -> None:
        fraction = max(0.0, min(1.0, float(fraction)))
        # Never let the bar jump backwards during a run (esptool phase lines can).
        if self._busy and fraction < self._progress_value:
            fraction = self._progress_value
        self._progress_value = fraction
        self._progress["value"] = int(round(fraction * 1000))
        if text is not None:
            self._progress_text.set(text)
        if fraction <= 0.001 and not self._busy:
            self._progress_pct.set("")
        else:
            self._progress_pct.set(f"{int(round(fraction * 100))}%")

    def _append_log(self, text: str) -> None:
        self._log.insert(tk.END, text)
        self._log.see(tk.END)

    def _drain_log(self) -> None:
        try:
            while True:
                self._append_log(self._log_q.get_nowait())
        except queue.Empty:
            pass
        latest: tuple[float, str] | None = None
        try:
            while True:
                latest = self._progress_q.get_nowait()
        except queue.Empty:
            pass
        if latest is not None:
            self._set_progress(latest[0], latest[1])
            self._set_status(latest[1], dot=C["sun"])
        self.after(80, self._drain_log)

    def start_flash(self) -> None:
        if self._busy:
            return
        port = self._selected_port()
        images = Path(self._images.get().strip()) if self._images.get().strip() else None
        if not port:
            messagebox.showwarning(
                "Choose a USB port",
                "Plug in your Buddy, click Find ports, then select the USB port.",
            )
            return
        if not images or not images_complete(images):
            missing = missing_images(images) if images else [
                "bootloader.bin",
                "partition-table.bin",
                "alpha_smart_neo2_buddy.bin",
                "littlefs.bin",
            ]
            if self._adv_dialog is None or not self._adv_dialog.winfo_exists():
                self._open_advanced()
            messagebox.showerror(
                "Firmware not ready",
                "This folder doesn’t have a complete firmware set.\n\n"
                f"Missing: {', '.join(missing)}\n\n"
                "Open Advanced and choose the releases/1.0.0 folder.",
                parent=self._adv_dialog or self,
            )
            return

        if self._manual.get():
            ok = messagebox.askokcancel(
                "Download mode",
                "Make sure the board is in download mode:\n\n"
                "1. Hold BOOT (BUT1)\n"
                "2. Press and release RESET (RST1)\n"
                "3. Let go of BOOT\n\n"
                "Click OK when ready — install starts right away.",
            )
            if not ok:
                return

        try:
            baud = int(self._baud.get())
        except ValueError:
            baud = 460800

        self._busy = True
        self._progress_value = 0.0
        self._set_progress(0.0, "Starting install…")
        self._flash_btn.configure(
            state=tk.DISABLED,
            bg=C["line"],
            fg=C["muted"],
            highlightbackground=C["line"],
        )
        self._set_status("Installing… this can take a minute", dot=C["sun"])
        self._append_log("\n—— Starting install ——\n")

        thread = threading.Thread(
            target=self._run_flash,
            args=(
                port,
                images,
                baud,
                self._manual.get(),
                self._verify.get(),
                self._wipe_config.get(),
            ),
            daemon=True,
        )
        thread.start()

    def _run_flash(
        self,
        port: str,
        images: Path,
        baud: int,
        manual: bool,
        do_verify: bool,
        wipe_config: bool,
    ) -> None:
        tracker = FlashProgressTracker(image_sizes(images))
        writer = _QueueWriter(self._log_q, self._progress_q, tracker)
        old_out, old_err = sys.stdout, sys.stderr
        sys.stdout = writer  # type: ignore[assignment]
        sys.stderr = writer  # type: ignore[assignment]

        def flash_ok() -> None:
            self._progress_q.put(tracker.set_phase("Firmware written to the board", 0.92))
            self._log_q.put("\nInstall finished writing to the board.\n")
            if not do_verify:
                self.after(0, lambda: self._flash_done(True, "Install complete"))
                return
            self._log_q.put("\n—— Checking that Buddy started ——\n")
            self._progress_q.put(tracker.set_phase("Checking that Buddy started…", 0.95))
            self.after(0, lambda: self._set_status("Checking that Buddy started…", dot=C["sun"]))
            try:
                boot_text = verify_boot(port, reset_first=True, log=writer.write)
                help_text = format_connection_help(parse_connection_info(boot_text))
                self._log_q.put("\n" + help_text + "\n")
                self._progress_q.put(tracker.set_phase("All set — Buddy is running", 1.0))
                self.after(0, lambda h=help_text: self._flash_done(True, "All set — Buddy is running", h))
            except SystemExit as exc:
                code = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
                if code == 0:
                    try:
                        boot_text = verify_boot(port, reset_first=False, log=writer.write)
                        help_text = format_connection_help(parse_connection_info(boot_text))
                        self._log_q.put("\n" + help_text + "\n")
                        self._progress_q.put(tracker.set_phase("All set — Buddy is running", 1.0))
                        self.after(
                            0, lambda h=help_text: self._flash_done(True, "All set — Buddy is running", h)
                        )
                        return
                    except Exception as inner:
                        self._log_q.put(f"\nCould not confirm startup: {inner}\n")
                        self.after(0, lambda: self._flash_done(False, "Installed, but startup check failed"))
                        return
                self._log_q.put(f"\nCould not confirm startup (tool exit {code}).\n")
                self.after(0, lambda: self._flash_done(False, "Installed, but startup check failed"))
            except Exception as exc:
                self._log_q.put(f"\nCould not confirm startup: {exc}\n")
                self.after(0, lambda: self._flash_done(False, "Installed, but startup check failed"))

        try:
            flash_firmware(
                port,
                images,
                baud=baud,
                manual_boot=manual,
                wipe_config=wipe_config,
                log=writer.write,
            )
            flash_ok()
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
            if code == 0:
                flash_ok()
            else:
                self._log_q.put("\nInstall did not finish successfully.\n")
                self.after(0, lambda: self._flash_done(False, "Install failed"))
        except Exception as exc:
            self._log_q.put(f"\nSomething went wrong: {exc}\n")
            self.after(0, lambda: self._flash_done(False, "Install failed"))
        finally:
            sys.stdout = old_out
            sys.stderr = old_err

    def _flash_done(self, ok: bool, status: str, connection_help: str | None = None) -> None:
        self._busy = False
        self._flash_btn.configure(
            state=tk.NORMAL,
            bg=C["coral"],
            fg=C["cream"],
            highlightbackground=C["ink"],
        )
        if ok:
            self._set_progress(1.0, status)
        else:
            # Keep whatever progress we reached; label the failure.
            self._progress_text.set(status)
            if self._progress_value <= 0:
                self._progress_pct.set("")
        self._set_status(status, dot=C["ok"] if ok else C["coral"])
        if ok:
            verified = "running" in status.lower() or "all set" in status.lower()
            if verified and connection_help:
                body = "Firmware is on the board and Buddy started up.\n\n" + connection_help
            elif connection_help:
                body = "Firmware is on the board.\n\n" + connection_help
            elif self._uart_only_profile:
                body = (
                    "Firmware is on the board.\n\n"
                    "This is a UART-only profile (no Wi‑Fi portal).\n"
                    "Open a serial terminal at 115200 baud, then type login <password> and help."
                )
            else:
                body = (
                    "Firmware is on the board.\n\n"
                    "Connect to the Buddy Wi‑Fi and open the portal in your browser.\n"
                    "First setup is usually http://192.168.4.1/"
                )
            messagebox.showinfo("You’re all set", body)
        elif "startup" in status.lower():
            messagebox.showwarning(
                "Almost there",
                "The firmware was written, but we couldn’t confirm Buddy started.\n\n"
                "Press the RESET button on the board, wait a few seconds, and try again "
                "with “Check that Buddy started” turned on.\n\n"
                "You can also open Show details for more information.",
            )
        else:
            messagebox.showerror(
                "Install didn’t finish",
                "Something went wrong while writing the firmware.\n\n"
                "Try this:\n"
                "• Put the board in download mode again (hold BOOT, tap RESET, release BOOT)\n"
                "• Confirm Advanced → “Board is already in download mode” is on\n"
                "• In Advanced, set Install speed to 115200\n\n"
                "Open Show details if you want the full log.",
            )


def main() -> None:
    app = FlasherApp()
    app.mainloop()
