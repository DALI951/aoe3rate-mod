"""
parser.py — turn one raw log line into (t_sec, {resource: value}).

No config dependency, no state. Pure function, easy to unit test with a
list of strings.
"""

from typing import Dict, Optional, Tuple


def parse_line(line: str) -> Optional[Tuple[float, Dict[str, float]]]:
    """Parse 't=<ms>,food=<v>,wood=<v>,coin=<v>,export=<v>'.

    Returns (t_sec, {resource: value}), or None if the line isn't a valid
    data line (blank, malformed, partial write mid-flush, etc.) — callers
    should just skip None and keep going, never raise on bad input.
    """
    line = line.strip()
    if not line or "=" not in line:
        return None

    fields: Dict[str, float] = {}
    for part in line.split(","):
        part = part.strip()
        if not part or "=" not in part:
            return None
        key, _, val = part.partition("=")
        key = key.strip()
        val = val.strip()
        try:
            fields[key] = float(val)
        except ValueError:
            return None

    if "t" not in fields:
        return None

    t_sec = fields.pop("t") / 1000.0  # ms -> sec
    if not fields:
        return None

    return t_sec, fields
