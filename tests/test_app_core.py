#!/usr/bin/env python3
"""test_app_core.py — R21 app-core tests (PASS/FAIL + exit code).

Covers the pure pipeline that must stay correct: parser.parse_line,
log_tailer.follow (temp-file tails), and engine.RateEngine math vs Dali's
config semantics. All Python, no GUI, no DLL needed.
"""
import os
import queue
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app")
sys.path.insert(0, APP)

import config          # noqa: E402
import engine          # noqa: E402
import log_tailer      # noqa: E402
import parser          # noqa: E402
from engine import RateEngine, format_rate  # noqa: E402

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"PASS: {msg}")
    else:
        print(f"FAIL: {msg}")
        failures += 1


_MISS = object()


# =====================================================================
# parser.parse_line
# =====================================================================
DALI_SAMPLES = [
    ("t=182340,food=100,wood=0,coin=0,export=0", (182.34, {"food": 100.0, "wood": 0.0, "coin": 0.0, "export": 0.0})),
    ("t=182840,food=120,wood=0,coin=0,export=0", (182.84, {"food": 120.0, "wood": 0.0, "coin": 0.0, "export": 0.0})),
    ("t=183340,food=120,wood=0,coin=0,export=0", (183.34, {"food": 120.0, "wood": 0.0, "coin": 0.0, "export": 0.0})),
]
for line, expect in DALI_SAMPLES:
    r = parser.parse_line(line)
    check(r is not None and abs(r[0] - expect[0]) < 1e-9 and r[1] == expect[1],
          f"parser: Dali sample {line!r} -> t={expect[0]}s {expect[1]}, got {r}")

r = parser.parse_line("t=5000,food=1024,wood=777,coin=65,export=33")
check(r is not None and r[0] == 5.0 and r[1] == {"food": 1024.0, "wood": 777.0, "coin": 65.0, "export": 33.0},
      f"parser: full 5-field line -> t=5.0s, got {r}")

check(parser.parse_line("") is None, "parser: empty line -> None")
check(parser.parse_line("   \n") is None, "parser: whitespace line -> None")
check(parser.parse_line("hello world") is None, "parser: garbage -> None")
check(parser.parse_line("food=100,wood=20") is None, "parser: missing t -> None")
check(parser.parse_line("t=1,food=10,co") is None, "parser: partial (unfinished field) -> None")
check(parser.parse_line("t = 5, food = 10") == (0.005, {"food": 10.0}),
      "parser: extra spaces around '=' are tolerated")
check(parser.parse_line("t=10,food=5,wood=6,coin=7,export=-3")[1]["export"] == -3.0,
      "parser: negative export parses (-3.0)")
check(parser.parse_line("t=abc,food=2") is None, "parser: non-numeric t -> None")


# =====================================================================
# log_tailer.follow  (one dedicated consumer thread -> queue, like the app)
# =====================================================================
tmpdir = tempfile.mkdtemp(prefix="rre_tail_")
path_tail = os.path.join(tmpdir, "rates.log")
with open(path_tail, "w", encoding="utf-8") as f:
    f.write("t=1,food=10\n")  # pre-existing content — must NOT be replayed

q = queue.Queue()
it = log_tailer.follow(path_tail, poll_sec=0.02)
threading.Thread(target=lambda: [q.put(x) for x in it], daemon=True).start()
time.sleep(0.15)  # let the tailer open the file + seek to EOF first


def qtake(timeout=2.0):
    try:
        return q.get(timeout=timeout)
    except queue.Empty:
        return _MISS


with open(path_tail, "a", encoding="utf-8") as f:
    f.write("t=2,food=20\n")
    f.flush()
got = qtake()
check(got == "t=2,food=20", f"tailer: starts at EOF, new line only, got {got!r}")
got2 = qtake(timeout=0.5)
check(got2 is _MISS, f"tailer: no duplicate replay when idle, got {got2!r}")

# partial write mid-flush: line without trailing newline is held back
with open(path_tail, "a", encoding="utf-8") as f:
    f.write("t=3,food=30")  # no newline yet
    f.flush()
got3 = qtake(timeout=0.5)
check(got3 is _MISS, f"tailer: partial line NOT yielded, got {got3!r}")
with open(path_tail, "a", encoding="utf-8") as f:
    f.write("\n")
    f.flush()
got4 = qtake()
check(got4 == "t=3,food=30", f"tailer: completed partial line yielded, got {got4!r}")

# missing file: survives absence, then reads once the file appears
path_missing = os.path.join(tmpdir, "later.log")
if os.path.exists(path_missing):
    os.remove(path_missing)
q5 = queue.Queue()
it5 = log_tailer.follow(path_missing, poll_sec=0.02)
threading.Thread(target=lambda: [q5.put(x) for x in it5], daemon=True).start()


def q5take(timeout=2.0):
    try:
        return q5.get(timeout=timeout)
    except queue.Empty:
        return _MISS


got5 = q5take(timeout=0.3)
check(got5 is _MISS, "tailer: missing-file survival (waits, does not raise)")
# file appears (empty first so the tailer opens it and seeks to EOF), then grows
with open(path_missing, "w", encoding="utf-8") as f:
    pass
time.sleep(0.1)  # let the tailer open the (empty) file and seek to its EOF
with open(path_missing, "a", encoding="utf-8") as f:
    f.write("t=9,food=90\n")
    f.flush()
got6 = q5take()
check(got6 == "t=9,food=90", f"tailer: picks up file created after start, got {got6!r}")


# =====================================================================
# engine.RateEngine
# =====================================================================
cfg = config.Config()  # pristine defaults from Dali's config.py
eng = RateEngine(cfg)


def feed_constant(engine, rate_per_sec, dt=0.5, base_value=0.0, t0=0.0, n=80, res="food"):
    """Feed constant income: value rises `rate_per_sec` every dt."""
    t = t0
    v = base_value
    engine.update(t, {res: v})
    for _ in range(n):
        t += dt
        v += rate_per_sec * dt
        engine.update(t, {res: v})
    return engine


rec = feed_constant(eng, rate_per_sec=2.0).last["resources"]["food"]
check(abs(rec["rate"] - 2.0) < 0.01,
      f"engine: constant 2/sec income, EMA converges to ~2.0 (got {rec['rate']:.4f})")
check(abs(rec["rate_min"] - rec["rate"] * 60.0) < 1e-9,
      "engine: rate_min == rate * 60 exactly")
check(rec["spend_count"] == 0, "engine: constant income -> no spend events")
check(abs(rec["rate"] - 2.0) < 0.01 and rec["value"] == 80.0,
      f"engine: raw value exposed and rate still ~2.0 (value={rec['value']})")

# dt < min_dt_sec: an ultra-close sample is ignored (no EMA jump, no spend)
t_before = eng._t_last["food"]
ema_before = eng.rate("food")
eng.update(t_before + 0.01, {"food": 500.0})
r_clos = eng.last["resources"]["food"]
check(abs(eng.rate("food") - ema_before) < 1e-9 and r_clos["spend_count"] == 0,
      "engine: dt < min_dt_sec sample ignored (EMA unchanged, no spend)")

# single huge negative spike: |instant| > |ema| * spend_ratio -> SPEND
eng2 = RateEngine(cfg)
eng2 = feed_constant(eng2, rate_per_sec=2.0, n=80)
ema_hi = eng2.rate("food")
# value was 160.0 at t=40.0; crash it to 10.0 over dt=0.5 -> instant -300/s
t_now = eng2.last_t
eng2.update(t_now + 0.5, {"food": 10.0})
rec_sp = eng2.last["resources"]["food"]
check(rec_sp["spend_count"] == 1,
      "engine: huge instant drop counted as ONE spend event")
check(abs(rec_sp["rate"] - ema_hi) < 0.1,
      f"engine: spend excluded from EMA (EMA stays ~{ema_hi:.2f}, not -300; got {rec_sp['rate']:.2f})")
check(eng2.spent("food") == 1.0, "engine: spent() reports total spend events")

# spending rule detail: small drops (|inst| <= |ema|*ratio) fold normally
eng3 = RateEngine(cfg)
eng3 = feed_constant(eng3, rate_per_sec=2.0, n=80)
ema3_before = eng3.rate("food")
t3 = eng3.last_t
eng3.update(t3 + 0.5, {"food": 79.0})  # drop of 1 over 0.5s -> instant -2/s
rec_small = eng3.last["resources"]["food"]
check(rec_small["spend_count"] == 0,
      "engine: small drop within spend_ratio folds (no spend tag)")
check(rec_small["rate"] < ema3_before and rec_small["rate"] > -2.0,
      f"engine: small drop folds into EMA (partially pulled down, got {rec_small['rate']:.3f})")


# display helpers via format_rate — default CONFIG display settings
d0 = config.Config()
d0.decimals = 1
d0.show_plus_sign = True
d0.zero_epsilon = 0.01
check(format_rate(2.0, config=d0) == "+2.0/sec", f"display: +2.0/sec, got {format_rate(2.0, config=d0)!r}")
check(format_rate(-2.0, config=d0) == "-2.0/sec", f"display: -2.0/sec (plus only on +ve), got {format_rate(-2.0, config=d0)!r}")
check(format_rate(2.0, unit="min", config=d0) == "+120.0/min", f"display: min unit *60, got {format_rate(2.0, unit='min', config=d0)!r}")
check(format_rate(0.005, config=d0) == "", "display: |rate|<epsilon and show_zero=False -> empty")
d0.show_zero = True
check(format_rate(0.0, config=d0) == "0.0/sec", f"display: show_zero=True -> 0.0/sec, got {format_rate(0.0, config=d0)!r}")
d0.display = None

# engine record carries formatted + formatted_min honouring the same rules
rec_fmt = feed_constant(RateEngine(config.Config()), rate_per_sec=2.0).last["resources"]["food"]
check("+120" in rec_fmt.get("formatted_min", "") and "+2" in rec_fmt.get("formatted", ""),
      f"engine record exposes formatted (+2.0/sec) + formatted_min (+120.0/min), got {rec_fmt['formatted']!r} / {rec_fmt['formatted_min']!r}")


# smoothing presets
c = config.Config()
c.set_smoothing_preset("low")
check(c.smoothing_tau_sec == 1.5, "preset: low -> tau 1.5")
c.set_smoothing_preset("MeDiUm")
check(c.smoothing_tau_sec == 3.5, "preset: case-insensitive 'MeDiUm' -> tau 3.5")
c.set_smoothing_preset("high")
check(c.smoothing_tau_sec == 8.0, "preset: high -> tau 8.0")
try:
    c.set_smoothing_preset("ultra")
    check(False, "preset: unknown name -> ValueError")
except ValueError:
    check(True, "preset: unknown name -> ValueError")

# reset()
r_eng = RateEngine(cfg)
feed_constant(r_eng, rate_per_sec=2.0)
r_eng.reset()
feed_constant(r_eng, rate_per_sec=2.0)
check(abs(r_eng.rate("food") - 2.0) < 0.01,
      "engine: reset() clears state, next series re-converges")

# =====================================================================
# R25: app path auto-detect + liveness helpers (app.py — pure, no GUI/DLL)
# =====================================================================
import app as appmod  # noqa: E402

r25 = tempfile.mkdtemp(prefix="rre_r25_")
mk = lambda n: os.path.join(r25, f"r25_{n}.log")  # noqa: E731
for _n in ("primary", "fb1", "fb2", "dead"):
    with open(mk(_n), "w", encoding="utf-8"):
        pass
now = time.time()
os.utime(mk("primary"), (now - 1.0, now - 1.0))          # fresh (test 2 uses it)
os.utime(mk("fb1"),     (now - 120.0, now - 120.0))      # stale (2min)
os.utime(mk("fb2"),     (now - 600.0, now - 600.0))      # stale for test 1; refreshed later
os.utime(mk("dead"),    (now - 600.0, now - 600.0))      # dead (10min)

_orig_fb = appmod._FALLBACK_RATES
appmod._FALLBACK_RATES = [mk("fb1"), mk("fb2"), mk("dead")]
no = os.path.join(r25, "r25_missing.log")
if os.path.exists(no):
    os.remove(no)

# 1) nothing exists/fresh -> None (keep polling, don't lock in)
got = appmod._pick_log_path(no, now=now)
check(got is None, f"R25 path: nothing fresh -> None (poll), got {got!r}")

# 2) primary (configured/--log) fresh wins over everything
got = appmod._pick_log_path(mk("primary"), now=now)
check(got == mk("primary"),
      f"R25 path: fresh configured path outranks fallbacks, got {got!r}")

# 3) primary stale/absent -> first FRESH fallback wins (order matters: fb1 old, fb2 fresh)
os.utime(mk("fb2"), (now - 3.0, now - 3.0))              # fb2 now fresh
got = appmod._pick_log_path(no, now=now)
check(got == mk("fb2"),
      f"R25 path: stale fb1 skipped -> fresh fb2 wins, got {got!r}")

# 4) configured path went STALE but a fresh fallback exists -> the fresh WINS
#    (a dead configured path must not hold the app hostage)
os.utime(mk("primary"), (now - 500.0, now - 500.0))    # primary now stale/dead
got = appmod._pick_log_path(mk("primary"), now=now)
check(got == mk("fb2"),
      f"R25 path: stale/dead configured path does not beat a fresh fallback, got {got!r}")
os.utime(mk("primary"), (now - 1.0, now - 1.0))        # restored

# 5) nothing fresh anywhere but the configured path EXISTS -> honor it (last resort)
os.utime(mk("primary"), (now - 500.0, now - 500.0))   # stale too now
os.utime(mk("fb2"),     (now - 500.0, now - 500.0))
os.utime(mk("fb1"),     (now - 500.0, now - 500.0))
got = appmod._pick_log_path(mk("primary"), now=now)
check(got == mk("primary"),
      f"R25 path: stale configured path used when NO fallback is fresh, got {got!r}")
os.utime(mk("fb1"), (now - 3.0, now - 3.0))             # restore fb1 fresh
os.utime(mk("fb2"), (now - 3.0, now - 3.0))             # restore fb2 fresh

# re-check each poll: fresh source appearing later is picked up, not locked out
got = appmod._pick_log_path(no, now=now)
check(got == mk("fb1"),
      f"R25 path: re-probe picks the first now-fresh fallback, got {got!r}")
appmod._FALLBACK_RATES = _orig_fb                     # restore

# 6) liveness footer states
t = appmod.time.time()
check(appmod._staleness_text(False, None, now=t) == "none",
      "R25 liveness: no data yet -> none (Waiting for data)")
check(appmod._staleness_text(True, t - 1.0, now=t) == "live",
      "R25 liveness: fresh last record -> live (shows t=)")
check(appmod._staleness_text(True, t - 5.0, now=t) == "live",
      "R25 liveness: exactly at the 5s threshold -> still live")
check(appmod._staleness_text(True, t - 5.1, now=t) == "stale",
      "R25 liveness: past 5s -> stale (no new samples)")
check(appmod._staleness_text(True, t - 300.0, now=t) == "stale",
      "R25 liveness: long gap -> stale, values never blanked")

print("APP-CORE:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)