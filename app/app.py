"""
app.py — resource-rate live viewer built ENTIRELY on Dali's pipeline.

Threading is (config.py + log_tailer.py + parser.py) + engine.py, imported
verbatim — nothing else computes a number or knows a path. The DLL writes
rates.log (one `t=<ms>,food=..,wood=..,coin=..,export=..` line every ~500ms);
this app tails that file, turns each line into (t_sec, {resource: value}),
feeds the RateEngine, and repaints a frameless always-on-top window at
CONFIG.refresh_hz showing Food/Wood/Coin/Export live values + GREEN/RED rates
(income vs spend) + a small `t=` timestamp footer.

Run:
    pythonw.exe app\\app.py            # GUI (headless-safe: console fallback)
    python  app\\app.py --console      # print lines to stdout instead
    pythonw.exe app\\app.py --log C:\\other\\rates.log
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)  # makes `python app\\app.py` work from repo root

from config import CONFIG            # noqa: E402
from filer import follow             # noqa: E402  (robust reader, ROUND 22)
from parser import parse_line        # noqa: E402
from engine import RateEngine        # noqa: E402

import datetime                      # noqa: E402
import threading                     # noqa: E402
import time                          # noqa: E402
import traceback                     # noqa: E402

ERROR_LOG = os.path.join(_HERE, "app.error.log")


def _log_error(exc, where):
    """Append a timestamped failure to app.error.log (pythonw shows nothing)."""
    try:
        with open(ERROR_LOG, "a", encoding="utf-8") as f:
            f.write(f"\n[{datetime.datetime.now():%Y-%m-%d %H:%M:%S}] "
                    f"ERROR in {where}\n")
            f.write(f"{type(exc).__name__}: {exc}\n")
            f.write(traceback.format_exc())
    except Exception:
        pass

# ---- look & feel (dark cinema, one accent; see taste skill) ---------------
BG = "#0a0c0f"
LABEL_FG = "#8899aa"
FG = "#e8e8e8"
RES_COLORS = {"food": "#e0c080", "wood": "#9ecfff",
              "coin": "#ffd76a", "export": "#a5e8a0"}
GREEN = "#7fd68a"
RED = "#ff7c6b"
DIM = "#3a4050"
T_FG = "#556677"
FONT = ("Consolas", 16, "bold")


def _run_loop(stdout_emit, engine):
    """Tail CONFIG.log_path -> parse -> engine.update -> emit(record) per line.
    Runs in its own thread so the GUI timer is what repaints. Any fault is
    logged (not silent under pythonw) and the loop keeps polling."""
    while True:
        try:
            for raw in follow(CONFIG.log_path, CONFIG.tail_poll_sec):
                try:
                    parsed = parse_line(raw)
                    if parsed is None:
                        continue
                    t_sec, values = parsed
                    engine.update(t_sec, values)
                    stdout_emit(engine)
                except Exception as exc:
                    _log_error(exc, "_run_loop(line=%r)" % raw[:80])
                    continue
        except Exception as exc:
            _log_error(exc, "_run_loop(follow)")
            time.sleep(1.0)


def emit_console(engine):
    """One status line per Engine.update, showing every resource."""
    t = engine.last_t
    parts = []
    for res, rec in engine.raw.get("resources", {}).items():
        v = rec["value"]
        r = rec["formatted"] or "0"
        sp = f"  SPEND x{rec['spend_count']}" if rec["spend_count"] else ""
        parts.append(f"{res}={v:.0f} {r}{sp}")
    print(f"t={t:8.2f}s  " + "  ".join(parts))


def build_window(items_tk, engine, log_path):
    """Return the Tk root + per-resource (value label, rate label) widgets."""
    tk = items_tk

    root = tk.Tk()
    root.overrideredirect(True)
    try:
        root.attributes("-topmost", True)
        root.attributes("-alpha", 0.85)
    except tk.TclError:
        pass
    root.configure(bg=BG)

    rows = []
    for res in CONFIG.resources:
        color = RES_COLORS.get(res, FG)
        row = tk.Frame(root, bg=BG)
        row.pack(fill="x", padx=8, pady=(4, 1))
        lab = tk.Label(row, text=res.upper() + "   ", bg=BG, fg=LABEL_FG,
                       font=FONT)
        lab.pack(side="left")
        val = tk.Label(row, text="...", bg=BG, fg=color, font=FONT)
        val.pack(side="left")
        rate = tk.Label(row, text="", bg=BG, fg=GREEN, font=FONT)
        rate.pack(side="left")
        rows.append((res, val, rate))

    foot = tk.Label(root, text=f"waiting for {log_path}…", bg=BG, fg=T_FG,
                    font=("Consolas", 8))
    foot.pack(side="bottom", padx=6, pady=(0, 4))

    # right-click close; left-drag move (whole window)
    root.bind("<Button-3>", lambda _e: root.destroy())
    drag = {"x": 0, "y": 0}

    def press(ev):
        drag["x"], drag["y"] = ev.x_root - root.winfo_x(), \
                               ev.y_root - root.winfo_y()

    def move(ev):
        root.geometry(f"+{ev.x_root - drag['x']}+{ev.y_root - drag['y']}")

    for w in [root] + [v for _r, v, _rt in rows] + [foot]:
        w.bind("<Button-1>", press)
        w.bind("<B1-Motion>", move)

    last = {}   # resource -> last good record (survives momentary gaps)

    def poke():
        rec = engine.raw
        res_map = rec.get("resources", {})
        for res, val_lab, rate_lab in rows:
            r = res_map.get(res)
            if r is not None:
                last[res] = r                              # remember last good
            r = last.get(res)
            if r is None:
                val_lab.configure(text="…")                # never blank-out:
                rate_lab.configure(text="")               # keep last known once
                continue                                  # we have data
            val_lab.configure(text=f"{r['value']:.0f}")
            txt = r["formatted"] or "0"
            fg = RED if r["rate"] < 0 else GREEN
            rate_lab.configure(text=txt, fg=fg)
        if res_map:
            foot.configure(text=f"t={rec['t']:.2f}s — {log_path}")
        else:
            foot.configure(text="Waiting for data… (start a match)",
                           fg=LABEL_FG)
        root.after(int(1000.0 / CONFIG.refresh_hz), poke)

    root.after(int(1000.0 / CONFIG.refresh_hz), poke)
    root.update_idletasks()
    sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
    ww, wh = root.winfo_reqwidth(), root.winfo_reqheight()
    root.geometry(f"{ww}x{wh}+{sw - ww - 12}+12")
    return root


def run_gui(engine, log_path):
    try:
        import tkinter as tk
    except Exception as exc:  # headless-safe fallback
        print(f"[app] Tkinter unavailable ({exc}) — falling back to console "
              f"output.", file=sys.stderr)
        run_console(engine)
        return

    root = build_window(tk, engine, log_path)
    t = threading.Thread(target=_run_loop, args=(emit_console, engine),
                         daemon=True)
    t.start()
    root.mainloop()


def run_console(engine):
    print(f"reading {CONFIG.log_path}…")
    t = threading.Thread(target=_run_loop, args=(emit_console, engine),
                         daemon=True)
    t.start()
    waited = 0
    while not engine.raw:
        if waited % 10 == 0:
            print("waiting for data… (start a match)", flush=True)
        time.sleep(0.2)
        waited += 1
    # keep running: _run_loop prints every line on its own thread
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass


def main(argv):
    log_path = CONFIG.log_path
    console = False
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--log" and i + 1 < len(argv):
            log_path = argv[i + 1]
            CONFIG.log_path = log_path
            i += 2
            continue
        if arg == "--console":
            console = True
            i += 1
            continue
        i += 1

    engine = RateEngine(CONFIG)
    if console:
        run_console(engine)
    else:
        run_gui(engine, log_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))