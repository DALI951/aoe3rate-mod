#!/usr/bin/env python3
"""test_widget_parse.py — R19 widget parse-line tests (PASS/FAIL + exit code).

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


SAMPLE = "food:120 ,wood:340 ,coin:500"

# normal sample
r = rates_widget.parse_line(SAMPLE)
check(r == (120, 340, 500), f"parse_line(sample) == (120,340,500), got {r}")

# whitespace-flexible variants (existing regex has \s* before comma; the DLL
# never emits a leading space before a value, only before the comma)
for variant in [
    "food:1,wood:2,coin:3",
    "food:1 ,wood:2 ,coin:3",
    "food:10 ,  wood:20, coin:30",
]:
    r = rates_widget.parse_line(variant)
    check(r is not None, f"whitespace variant parses: {variant!r}")

# leading space before a VALUE is NOT part of the DLL format -> None (kept)
check(rates_widget.parse_line("food: 10 ,wood:2 ,coin:3") is None,
      "leading space before a value -> None (never emitted by the DLL)")

# embedded in a longer line (tail may carry other text)
r = rates_widget.parse_line("junk food:7 ,wood:8 ,coin:9 tail")
check(r == (7, 8, 9), f"embedded in extra text: got {r}")

# malformed / absent
check(rates_widget.parse_line("") is None, "empty line -> None")
check(rates_widget.parse_line("hello") is None, "non-export line -> None")
check(rates_widget.parse_line("food:abc ,wood:2 ,coin:3") is None,
      "non-integer food -> None")
check(rates_widget.parse_line("RES t=1 food=0 wood=0 coin=0") is None,
      "old RES format (no export separators) -> None")
check(rates_widget.parse_line(None) is None, "None -> None")

# missing fields
check(rates_widget.parse_line("food:1 ,wood:2") is None,
      "missing coin -> None")

# regex module-level PATTERN exists and matches the sample
check(rates_widget.PATTERN is not None and rates_widget.PATTERN.search(SAMPLE),
      "module-level PATTERN matches the sample line")

# float-locale note: a localised comma-decimal ("food:1,5") cannot be a valid
# export line — \d+ matches integer tokens only, so the line fails to parse
# (the comma is not followed by wood:). This is the documented locale-safe
# behaviour: the DLL exports INTEGERS, so no locale decimal comma can appear.
check(rates_widget.parse_line("food:1,5 ,wood:2 ,coin:3") is None,
      "comma-decimal 1,5 -> None (integers only, locale-safe)")

print("WIDGET-PARSE:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)
