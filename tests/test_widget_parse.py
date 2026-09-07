#!/usr/bin/env python3
"""test_widget_parse.py — R20 widget parse-line tests (PASS/FAIL + exit code).

Calls tools\\rates_widget.py parse_line / PATTERN against the sample export
line plus malformed cases, so the tail-display contract is pinned.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "rates_widget.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import rates_widget  # noqa: E402

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"PASS: {msg}")
    else:
        print(f"FAIL: {msg}")
        failures += 1


SAMPLE = "t=182340,food=100,wood=0,coin=0,export=0"

# normal sample
r = rates_widget.parse_line(SAMPLE)
check(r == (182340, 100, 0, 0, 0), f"parse_line(sample) == (182340,100,0,0,0), got {r}")

# Dali's exact three example lines from the R20 spec
for line, expect in [
    ("t=182340,food=100,wood=0,coin=0,export=0", (182340, 100, 0, 0, 0)),
    ("t=182840,food=120,wood=0,coin=0,export=0", (182840, 120, 0, 0, 0)),
    ("t=183340,food=120,wood=0,coin=0,export=0", (183340, 120, 0, 0, 0)),
]:
    r = rates_widget.parse_line(line)
    check(r == expect, f"Dali sample {line!r} -> {expect}, got {r}")

# all five fields filled
r = rates_widget.parse_line("t=5000,food=1024,wood=777,coin=65,export=33")
check(r == (5000, 1024, 777, 65, 33), f"all five fields: got {r}")

# embedded in a longer line (tail may carry other text)
r = rates_widget.parse_line("junk t=7,food=8,wood=9,coin=10,export=11 tail")
check(r == (7, 8, 9, 10, 11), f"embedded in extra text: got {r}")

# malformed / absent
check(rates_widget.parse_line("") is None, "empty line -> None")
check(rates_widget.parse_line("hello") is None, "non-export line -> None")
check(rates_widget.parse_line("t=abc,food=2,wood=3,coin=4,export=5") is None,
      "non-integer t -> None")
check(rates_widget.parse_line("t=1,food=abc,wood=3,coin=4,export=5") is None,
      "non-integer food -> None")
check(rates_widget.parse_line("RES t=1 food=0 wood=0 coin=0") is None,
      "old RES format (no comma separators) -> None")
check(rates_widget.parse_line("food:100 ,wood:0 ,coin:0") is None,
      "R19 format (no t prefix) -> None")
check(rates_widget.parse_line(None) is None, "None -> None")

# missing fields
check(rates_widget.parse_line("t=1,food=2,wood=3,coin=4") is None,
      "missing export -> None")

# regex module-level PATTERN exists and matches the sample
check(rates_widget.PATTERN is not None and rates_widget.PATTERN.search(SAMPLE),
      "module-level PATTERN matches the sample line")

print("WIDGET-PARSE:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)