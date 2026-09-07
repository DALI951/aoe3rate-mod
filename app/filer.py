"""
filer.py — robust log reader for the rates app (OURS, added in ROUND 22).

Dali's log_tailer.follow() has one blind spot: it opens the file once, seeks
to EOF, and reads from there — if the writer later TRUNCATES the file (the
DLL's export thread does fopen("w") once at start), the open handle's read
position lands beyond EOF and readline() returns '' forever -> the app looks
stuck. This module is the same philosophy (only NEW complete lines, poll-based,
missing-file safe, partial-line rewind) PLUS truncation/recreation detection:

  - file missing            -> poll until it appears (never crash)
  - first open of a file    -> seek to END (don't flash stale history)
  - size < last position    -> writer truncated/recreated it: reopen, seek to
                               EOF, continue from new content only
  - partial last line       -> rewind to line start and poll again until the
                               trailing newline lands

All of Dali's files (config.py / log_tailer.py / parser.py) stay byte-identical.
"""

import os
import time
from typing import Iterator, Optional

from config import CONFIG


def _resume_pos(f) -> int:
    """Position just after the last COMPLETE line: skip any torn tail. On a
    fresh/empty file or mid-write batch this lands at 0 / last '\n' so the
    reader resumes at line boundaries (matching the tailer's philosophy)."""
    f.seek(0, 2)
    size = f.tell()
    if size == 0:
        return 0
    f.seek(0)
    data = f.read()
    nl = data.rfind("\n")
    return (nl + 1) if nl >= 0 else 0


def follow(path: str, poll_sec: Optional[float] = None) -> Iterator[str]:
    poll_sec = CONFIG.tail_poll_sec if poll_sec is None else poll_sec
    f = None
    pos = 0
    while True:
        if f is None:
            try:
                f = open(path, "r", encoding="utf-8", errors="replace")
                pos = _resume_pos(f)  # only new content from here on
            except FileNotFoundError:
                time.sleep(poll_sec)
                continue
            except OSError:
                time.sleep(poll_sec)
                continue

        # truncation / recreation detection: writer shrank the file under us
        try:
            cur_size = os.path.getsize(path)
        except OSError:
            cur_size = -1
            time.sleep(poll_sec)
            continue

        if 0 <= cur_size < pos:
            # file was truncated or recreated -> reopen, jump to the last
            # complete line, keep going from NEW content only
            try:
                f.close()
            except Exception:
                pass
            f = None
            try:
                f = open(path, "r", encoding="utf-8", errors="replace")
                pos = _resume_pos(f)
            except OSError:
                time.sleep(poll_sec)
                continue

        line_start = f.tell()
        line = f.readline()

        if not line:
            time.sleep(poll_sec)
            continue

        if not line.endswith("\n"):
            # partial write mid-flush — rewind and wait for the rest
            f.seek(line_start)
            time.sleep(poll_sec)
            continue

        pos = f.tell()
        yield line.rstrip("\n")