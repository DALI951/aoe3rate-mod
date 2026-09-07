"""
log_tailer.py — follow a growing log file, yielding new lines as they land.

Starts at end-of-file (doesn't replay history), survives the file not
existing yet, survives partial lines mid-flush.
"""

import time
from typing import Iterator, Optional

from config import CONFIG


def follow(path: str, poll_sec: Optional[float] = None) -> Iterator[str]:
    poll_sec = CONFIG.tail_poll_sec if poll_sec is None else poll_sec
    f = None
    while True:
        if f is None:
            try:
                f = open(path, "r", encoding="utf-8", errors="replace")
                f.seek(0, 2)  # jump to end — only new content from here on
            except FileNotFoundError:
                time.sleep(poll_sec)
                continue

        pos = f.tell()
        line = f.readline()

        if not line:
            time.sleep(poll_sec)
            continue

        if not line.endswith("\n"):
            # caught a partial write mid-flush — rewind and wait for the rest
            f.seek(pos)
            time.sleep(poll_sec)
            continue

        yield line.rstrip("\n")
