"""
config.py — single source of truth for every Resource Rate Engine setting.

Nothing else in this codebase hardcodes a number or a path. Edit values
here, nothing else needs to change. If you want a settings GUI later, it
just needs to read/write this one object.
"""

from dataclasses import dataclass, field
from typing import List, Dict


@dataclass
class Config:
    # ---------------------------------------------------------------
    # INPUT — where the raw data comes from, what the DLL writes
    # ---------------------------------------------------------------
    # Expected line format the DLL must write, ONE per sample, its own file:
    #   t=182340,food=100,wood=0,coin=0,export=0
    # t = milliseconds, from the DLL's own monotonic clock (GetTickCount() or
    # QPC-derived ms). This is NOT optional — see note at bottom of file.
    log_path: str = r"C:\Users\dali\Documents\Age of Empires III - Complete Collection\rates.log"

    resources: List[str] = field(default_factory=lambda: ["food", "wood", "coin", "export"])

    # ---------------------------------------------------------------
    # RATE CALCULATION
    # ---------------------------------------------------------------
    smoothing_tau_sec: float = 3.5   # EMA time constant, in seconds. Bigger = smoother/laggier.
    spend_ratio: float = 3.0         # |inst| > |ema| * spend_ratio => treated as a spend, not income drop
    min_dt_sec: float = 0.05         # ignore two samples closer together than this (dedupe/jitter guard)

    # Named presets so a GUI can offer "Low/Medium/High" without hardcoding
    # numbers anywhere else. Use cfg.set_smoothing_preset("high").
    SMOOTHING_PRESETS: Dict[str, float] = field(default_factory=lambda: {
        "low": 1.5,
        "medium": 3.5,
        "high": 8.0,
    }, repr=False)

    def set_smoothing_preset(self, name: str) -> None:
        key = name.strip().lower()
        if key not in self.SMOOTHING_PRESETS:
            raise ValueError(f"unknown smoothing preset {name!r}, expected one of {list(self.SMOOTHING_PRESETS)}")
        self.smoothing_tau_sec = self.SMOOTHING_PRESETS[key]

    # ---------------------------------------------------------------
    # DISPLAY — pure formatting, never affects the underlying math
    # ---------------------------------------------------------------
    rate_unit: str = "sec"           # "sec" or "min" — display only, computed as ema * 60 for "min"
    decimals: int = 1
    show_plus_sign: bool = True
    show_zero: bool = False
    zero_epsilon: float = 0.01       # |rate| below this counts as "zero" for show_zero filtering

    # ---------------------------------------------------------------
    # REFRESH — independent of the DLL's write cadence
    # ---------------------------------------------------------------
    refresh_hz: float = 5.0          # console/GUI repaint rate
    tail_poll_sec: float = 0.05      # how often the tailer checks for new file content when idle


CONFIG = Config()

# ---------------------------------------------------------------------------
# NOTE on log_path's format requirement:
# The rate math needs REAL elapsed seconds between samples, not "assume
# 500ms passed." Disk flush timing, GUI/Python read latency, and dropped
# frames on the DLL side all make "sample N" != "N * 500ms" in practice.
# The DLL must stamp every line with its own clock (t=<ms>). Everything
# downstream trusts that timestamp, not the wall clock it was read at.
# ---------------------------------------------------------------------------
