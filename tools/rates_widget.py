#!/usr/bin/env python3
"""rates_widget.py — always-on-top frameless resource-rate viewer for
aoe3rate-mod (R19 pivot). Tails the d3d9mod.log export line
`food:X ,wood:X ,coin:X` and displays the live values.

Stdlib only (tkinter, re, time, os, sys). The parse logic lives in a pure
function `parse_line` (module-level PATTERN) so tests can import it.

Usage:
    pythonw.exe tools\\rates_widget.py [--log PATH] [--fps]

Default log path = the AoE3 TAD game dir d3d9mod.log.
"""
import os
import re
import sys
import time
import tkinter as tk

PATTERN = re.compile(r"food:(\d+)\s*,\s*wood:(\d+)\s*,\s*coin:(\d+)")

DEFAULT_LOG = (r"C:\Users\dali\Documents\Age of Empires III - Complete Collection"
               r"\d3d9mod.log")

FOOD_COLOR = "#e0c080"
WOOD_COLOR = "#9ecfff"
COIN_COLOR = "#ffd76a"


def parse_line(line):
    """Return (food, wood, coin) ints from one log line, or None if it is not
    a valid export line. Pure — no I/O, importable by tests."""
    if not line:
        return None
    m = PATTERN.search(line)
    if not m:
        return None
    try:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    except ValueError:
        return None


def tail_last_line(path, max_back=256):
    """Return the last non-empty tail of the file (or '' if absent/unreadable).
    Seeks near the end, reads back a window, returns the last non-empty line's
    content via a cheap textual index (-1 last newline)."""
    try:
        size = os.path.getsize(path)
    except OSError:
        return ""
    if size <= 0:
        return ""
    back = min(size, max_back)
    try:
        with open(path, "rb") as f:
            f.seek(size - back)
            data = f.read(back)
    except OSError:
        return ""
    text = data.decode("utf-8", "replace")
    lines = text.splitlines()
    for ln in reversed(lines):
        if ln.strip():
            return ln
    return ""


class RateWidget:
    def __init__(self, log_path, fps):
        self.log_path = log_path
        self.fps = fps is not None
        self.poll_ms = 200
        self.root = tk.Tk()
        self.root.overrideredirect(True)
        try:
            self.root.attributes("-topmost", True)
            self.root.attributes("-alpha", 0.85)
        except tk.TclError:
            pass
        self.root.configure(bg="#0a0c0f")
        self._last_food = self._last_wood = self._last_coin = ""
        self._fps_count = 0
        self._fps_time = time.time()
        self._fps_text = ""

        rows = ["food", "wood", "coin", "fps"] if fps else ["food", "wood", "coin"]
        self.row_vars = {}
        self.row_widgets = {}
        for i, name in enumerate(rows):
            color = {"food": FOOD_COLOR, "wood": WOOD_COLOR,
                     "coin": COIN_COLOR, "fps": "#8899aa"}[name]
            label = name.upper() + "   "
            row = tk.Frame(self.root, bg="#0a0c0f")
            row.pack(fill="x", padx=8, pady=(4 if i == 0 else 1, 1))
            lab = tk.Label(row, text=label, bg="#0a0c0f", fg="#8899aa",
                           font=("Consolas", 16, "bold"))
            lab.pack(side="left")
            val = tk.Label(row, text="", bg="#0a0c0f", fg=color,
                           font=("Consolas", 16, "bold"))
            val.pack(side="left")
            self.row_vars[name] = val
            self.row_widgets[name] = row

        # close on right-click (frameless has no close button)
        self.root.bind("<Button-3>", lambda e: self.root.destroy())

        # draggable via left-button drag on the frame
        self._drag = None

        def press(ev):
            self._drag = (ev.x_root - self.root.winfo_x(),
                          ev.y_root - self.root.winfo_y())

        def move(ev):
            if self._drag is not None:
                self.root.geometry(f"+{ev.x_root - self._drag[0]}"
                                   f"+{ev.y_root - self._drag[1]}")

        for w in [self.root] + list(self.row_widgets.values()):
            w.bind("<Button-1>", press)
            w.bind("<B1-Motion>", move)

        self._set_placeholder()
        self.root.after(self.poll_ms, self.poll)
        self.root.protocol("WM_DELETE_WINDOW", self.root.destroy)

    def _set_placeholder(self):
        for name, val in self.row_vars.items():
            val.configure(text="..." if name == "fps" else "")

    def poll(self):
        line = tail_last_line(self.log_path)
        parsed = parse_line(line) if line else None
        if parsed is not None:
            f, w, c = parsed
            self.row_vars["food"].configure(text=str(f))
            self.row_vars["wood"].configure(text=str(w))
            self.row_vars["coin"].configure(text=str(c))
        else:
            self._set_placeholder()
        if self.fps and "fps" in self.row_vars:
            self._fps_count += 1
            now = time.time()
            if now - self._fps_time >= 1.0:
                self._fps_text = str(round(self._fps_count / (now - self._fps_time)))
                self._fps_count = 0
                self._fps_time = now
            self.row_vars["fps"].configure(text=self._fps_text)
        self.root.after(self.poll_ms, self.poll)

    def run(self):
        # center on screen initially
        self.root.update_idletasks()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        ww = self.root.winfo_reqwidth()
        wh = self.root.winfo_reqheight()
        self.root.geometry(f"{ww}x{wh}+{sw - ww - 12}+12")
        self.root.mainloop()


def main(argv):
    log_path = DEFAULT_LOG
    fps = False
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--log" and i + 1 < len(argv):
            log_path = argv[i + 1]
            i += 2
            continue
        if arg == "--fps":
            fps = True
            i += 1
            continue
        i += 1
    RateWidget(log_path, fps).run()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
