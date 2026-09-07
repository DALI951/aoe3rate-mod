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
import queue                         # noqa: E402
import threading                     # noqa: E402
import time                          # noqa: E402
import traceback                     # noqa: E402

ERROR_LOG = os.path.join(_HERE, "app.error.log")

# ---- R25: log-path auto-detection ------------------------------------------
# config.py's log_path default predates this PC's move of the game install to
# Documents\gaames (note the missing "gaames" segment). Rather than touch Dali's
# config.py, the app resolves the ACTIVE path itself: the configured/--log path
# has priority, then the real game install here, then the shipped config default,
# then rates.log in the CWD. A path only wins if the file exists AND was written
# within FRESH_SEC — nothing is locked in, the choice is re-made every probe so a
# dead/stale path never holds the app hostage.
_DEFAULT_LOG_PATH = CONFIG.log_path          # the shipped config.py default
_FALLBACK_RATES = [
    r"C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\rates.log",
    _DEFAULT_LOG_PATH,
    os.path.join(os.getcwd(), "rates.log"),
]
FRESH_SEC = 60.0                              # file must have been written recently
_PROBE_SEC = 1.0                              # path re-check cadence in the reader
_STALE_SEC = 5.0                              # footer flips to "no new samples"


def _pick_log_path(primary=None, now=None):
    """The path to tail right now, or None to keep polling.

    1. If the configured/--log path (`primary`) exists AND is fresh it wins
       (explicit user intent outranks auto-detection).
    2. Otherwise scan the auto-fallback candidates in order and pick the FIRST
       whose file exists AND was written within FRESH_SEC — the real fix for
       this PC, where config.py's default predates the move of the install to
       Documents\\gaames.
    3. If nothing fresh exists but the configured path does, fall back to it
       (an explicit --log to an existing file is always honored) — the footer's
       liveness state makes staleness visible.
    4. Else None -> keep polling (nothing exists yet; the game has not been
       launched, or its log is old).
    """
    now = time.time() if now is None else now

    def fresh(path):
        try:
            return os.path.exists(path) and \
                   (now - os.path.getmtime(path)) <= FRESH_SEC
        except OSError:
            return False

    if primary is not None and fresh(primary):
        return primary
    for path in _FALLBACK_RATES:
        if path == primary:
            continue
        if fresh(path):
            return path
    try:
        if primary is not None and os.path.exists(primary):
            return primary
    except OSError:
        pass
    return None


def _staleness_text(has_data, last_at, now=None):
    """Footer text for the liveness state. has_data = engine has values;
    last_at = wall-clock time of the last accepted line (or None)."""
    now = time.time() if now is None else now
    if has_data and last_at is not None and (now - last_at) <= _STALE_SEC:
        return "live"
    if has_data:
        return "stale"
    return "none"


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


def _spawn_follow(path, out_q, tag):
    """Run filer.follow() on `path` in a daemon thread, pushing (tag, raw)
    tuples into out_q so the reader can switch paths without losing a line.
    On path change the old worker is simply abandoned (daemon; it drifts off
    when the stale path stops delivering — harmless)."""
    def run():
        try:
            for raw in follow(path, CONFIG.tail_poll_sec):
                out_q.put((tag, raw))
        except Exception:
            pass
    threading.Thread(target=run, daemon=True).start()


def _run_loop(stdout_emit, engine, status, announce=None):
    """Tail the ACTIVE rates.log path -> parse -> engine.update -> emit(record).

    The path is re-resolved every _PROBE_SEC against the candidate list, so the
    app follows the game's rates.log even when Dali's config.log_path is wrong
    for this PC or the file appears late — and never stays pinned to a stale
    path. `status` carries {path, last_at} for the GUI footer; `announce` (if
    any) is a console callback. Any fault is logged (not silent under pythonw)
    and the loop keeps polling."""
    out_q = queue.Queue()
    path = None
    tag = 0
    while True:
        try:
            now = time.time()
            want = _pick_log_path(CONFIG.log_path, now=now)
            if want != path:
                _announce_path_change(announce, path, want)
                path = want
                if path is not None:
                    tag += 1
                    _spawn_follow(path, out_q, tag)
            status["path"] = path
            if path is None:
                time.sleep(_PROBE_SEC)
                continue
            try:
                raw = out_q.get(timeout=_PROBE_SEC)
            except queue.Empty:
                continue
            if raw[0] != tag:
                continue           # stale worker line from the old path
            try:
                parsed = parse_line(raw[1])
                if parsed is None:
                    continue
                t_sec, values = parsed
                engine.update(t_sec, values)
                status["last_at"] = time.time()
                stdout_emit(engine)
            except Exception as exc:
                _log_error(exc, "_run_loop(line=%r)" % raw[1][:80])
                continue
        except Exception as exc:
            _log_error(exc, "_run_loop(follow)")
            time.sleep(1.0)


def _announce_path_change(announce, prev, now):
    """One console line per path change when --console is active."""
    if announce is None or prev == now:
        return
    if prev is None and now is not None:
        announce(f"[app] using rates.log: {now}")
    elif now is None:
        announce("[app] no fresh rates.log — polling candidates…")


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


def build_window(items_tk, engine, log_path, status):
    """Return the Tk root + per-resource (value, rate, spend) widgets.
    `status` (a dict shared with the reader thread) carries the ACTIVE path and
    the wall-clock time of the last accepted record for the liveness footer."""
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
        spend = tk.Label(row, text="", bg=BG, fg=RED, font=("Consolas", 10))
        spend.pack(side="left")
        rows.append((res, val, rate, spend))

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

    for w in [root] + [v for _r, v, _rt, _sp in rows] + [foot]:
        w.bind("<Button-1>", press)
        w.bind("<B1-Motion>", move)

    last = {}   # resource -> last good record (survives momentary gaps)

    def poke():
        rec = engine.raw
        res_map = rec.get("resources", {})
        for res, val_lab, rate_lab, spend_lab in rows:
            r = res_map.get(res)
            if r is not None:
                last[res] = r                              # remember last good
            r = last.get(res)
            if r is None:
                val_lab.configure(text="…")                # never blank-out:
                rate_lab.configure(text="")               # keep last known once
                spend_lab.configure(text="")              # we have data
                continue
            val_lab.configure(text=f"{r['value']:.0f}")
            txt = r["formatted"] or "0"
            fg = RED if r["rate"] < 0 else GREEN
            rate_lab.configure(text=txt, fg=fg)
            # R25 spend suffix: the engine records spent events/min per resource
            # (rec['spent_min']); show a red "-N/min spent" when nonzero this
            # window. If a record ever lacks the field, leave it empty.
            spent_min = r.get("spent_min", 0.0) or 0.0
            if spent_min > 0:
                spend_lab.configure(text=f"-{spent_min:.0f}/min spent")
            else:
                spend_lab.configure(text="")
        state = _staleness_text(bool(res_map), status.get("last_at"))
        active = status.get("path") or log_path
        if state == "live":
            foot.configure(text=f"t={rec['t']:.2f}s — {active}", fg=T_FG)
        elif state == "stale":
            foot.configure(text="waiting for rates.log … (no new samples)",
                           fg=T_FG)
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

    status = {"path": None, "last_at": None}
    root = build_window(tk, engine, log_path, status)
    t = threading.Thread(target=_run_loop, args=(emit_console, engine, status),
                         daemon=True)
    t.start()
    root.mainloop()


def run_console(engine):
    status = {"path": None, "last_at": None}
    print(f"reading {CONFIG.log_path}…", flush=True)
    t = threading.Thread(target=_run_loop,
                         args=(emit_console, engine, status, print),
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