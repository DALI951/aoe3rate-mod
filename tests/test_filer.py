"""
test_filer.py — ROUND 22: robust reader (app/filer.py) vs Dali's log_tailer.

Proves filer fixes the truncation stick: writer truncates + rewrites ->
filer recovers and keeps yielding NEW content only (never stale history),
missing file -> clean start when it appears, partial line -> held until \n.
"""

import os
import queue
import sys
import tempfile
import threading
import time

APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "app")
if APP not in sys.path:
    sys.path.insert(0, APP)

import filer  # noqa: E402

fails = 0
checks = 0


def check(cond, msg):
    global fails, checks
    checks += 1
    if cond:
        print(f"PASS: {msg}")
    else:
        fails += 1
        print(f"FAIL: {msg}")


def run_reader(path, poll=0.02):
    """One dedicated consumer thread -> queue, mirroring the app."""
    q = queue.Queue()
    it = filer.follow(path, poll_sec=poll)
    threading.Thread(target=lambda: [q.put(x) for x in it], daemon=True).start()
    return q


def qtake(q, timeout=2.0):
    try:
        return q.get(timeout=timeout)
    except queue.Empty:
        return None


tmpdir = tempfile.mkdtemp(prefix="rre_filer_")
p = os.path.join(tmpdir, "rates.log")

# ---- 1. growing file: seeds of pre-existing content must NOT be replayed ----
with open(p, "w", encoding="utf-8") as f:
    f.write("t=100,food=10,wood=5,coin=0,export=0\n")  # old history
q = run_reader(p)
time.sleep(0.15)  # reader opens + seeks to EOF first
with open(p, "a", encoding="utf-8") as f:
    f.write("t=600,food=12,wood=5,coin=0,export=0\n")
    f.flush()
got = qtake(q)
check(got == "t=600,food=12,wood=5,coin=0,export=0",
      f"filer: growing file — new line only, no replay (got {got!r})")

# ---- 2. TRUNCATION mid-stream (the R22 bug): recovers + continues ----------
# simulate the DLL's export_thread: fopen("w") truncates, then appends fresh.
with open(p, "w", encoding="utf-8") as f:    # game truncates at thread start
    pass
time.sleep(0.05)
with open(p, "a", encoding="utf-8") as f:    # then starts writing new session
    f.write("t=200,food=200,wood=60,coin=1,export=1\n")
    f.flush()
new = qtake(q, timeout=3.0)
check(new is not None,
      f"filer: survives truncation (got {new!r})")
check(new and new.endswith("export=1"),
      f"filer: truncation recovery — content is NEW data (got {new!r})")

# ---- 3. after truncation the stream keeps flowing ---------------------------
with open(p, "a", encoding="utf-8") as f:
    f.write("t=700,food=210,wood=62,coin=1,export=1\n")
    f.flush()
got3 = qtake(q)
check(got3 == "t=700,food=210,wood=62,coin=1,export=1",
      f"filer: stream continues after truncation (got {got3!r})")

# ---- 4. partial last line held until trailing newline -----------------------
with open(p, "a", encoding="utf-8") as f:
    f.write("t=1200,food=220,wood=64,coin=1,export=1")  # no newline
    f.flush()
got4 = qtake(q, timeout=0.4)
check(got4 is None, f"filer: partial line held back (got {got4!r})")
with open(p, "a", encoding="utf-8") as f:
    f.write("\n")
    f.flush()
got5 = qtake(q)
check(got5 == "t=1200,food=220,wood=64,coin=1,export=1",
      f"filer: partial completed after newline (got {got5!r})")

# ---- 5. missing file -> appears later -> clean start -------------------------
p2 = os.path.join(tmpdir, "later.log")
if os.path.exists(p2):
    os.remove(p2)
q2 = run_reader(p2)
got6 = qtake(q2, timeout=0.4)
check(got6 is None, f"filer: missing file — no crash, nothing yet (got {got6!r})")
with open(p2, "w", encoding="utf-8") as f:
    pass
time.sleep(0.1)  # reader opens + seeks EOF (empty)
with open(p2, "a", encoding="utf-8") as f:
    f.write("t=9,food=90,wood=3,coin=0,export=0\n")
    f.flush()
got7 = qtake(q2)
check(got7 == "t=9,food=90,wood=3,coin=0,export=0",
      f"filer: file created after start — clean first line (got {got7!r})")

# ---- 6. no infinite loop / no crash on garbage lines -------------------------
with open(p, "a", encoding="utf-8") as f:
    f.write("not a rate line\n")
    f.write("t=1800,food=230,wood=65,coin=2,export=1\n")
    f.flush()
got8 = qtake(q)
check(got8 == "not a rate line",
      f"filer: garbage lines are still yielded to the caller (got {got8!r})")
got9 = qtake(q)
check(got9 == "t=1800,food=230,wood=65,coin=2,export=1",
      f"filer: continues after garbage (got {got9!r})")

# ---- 7. heavyweight truncation: rewrite BEFORE the reader notices ------------
# Model the REAL case: a big previous-session file (like a 15 KB rates.log)
# gets truncated + refilled. Reader must survive + resume at new content.
def drain(q, wait=0.6):
    """Consume whatever the reader has queued so far (returns list)."""
    out = []
    while True:
        it = qtake(q, timeout=wait)
        if it is None:
            return out
        out.append(it)


long_history = "".join(f"t={i},food=999,wood=1,coin=0,export=0\n"
                       for i in range(0, 20000, 100))
with open(p, "w", encoding="utf-8") as f:
    f.write(long_history)
    f.flush()
time.sleep(0.15)          # reader resumes inside the big history
drain(q)                  # let it chew through the old history (growth)
with open(p, "w", encoding="utf-8") as f:
    f.write("t=2000,food=300,wood=70,coin=3,export=1\n"
            "t=2500,food=310,wood=72,coin=3,export=1\n"
            "t=3000,food=320,wood=74,coin=3,export=1\n")
    f.flush()
# truncation shrink (big -> small) forced a reopen -> must now reach new data
seen = qtake(q, timeout=3.0)
check(seen is None,
      f"filer: big-file truncation -> reopened at end, nothing stale replayed "
      f"(got {seen!r})")
drain(q)
with open(p, "a", encoding="utf-8") as f:
    f.write("t=3500,food=330,wood=76,coin=3,export=1\n")
    f.flush()
seen2 = qtake(q, timeout=3.0)
check(seen2 == "t=3500,food=330,wood=76,coin=3,export=1",
      f"filer: continues after big-file truncation recovery (got {seen2!r})")

print(f"\nFILER: {checks} checks -> {'PASS' if not fails else f'{fails} FAILURES'}")
sys.exit(1 if fails else 0)