"""
engine.py — RateEngine: per-resource EMA income/spend rate math.

Every number consumed here comes from the Config (config.py) — nothing is
hardcoded. Semantics per Dali's spec:

- Per-resource EMA income rate, alpha = 1 - exp(-dt / tau) with
  tau = CONFIG.smoothing_tau_sec (seconds). Bigger tau = smoother/laggier.
- Samples closer than CONFIG.min_dt_sec are ignored (dedupe/jitter guard).
- An instant delta is "income" when the value went UP (delta > 0), or when
  the drop is small: |instant| <= |ema| * CONFIG.spend_ratio. Those fold into
  the EMA. A larger drop (|instant| > |ema| * spend_ratio) is tagged a SPEND
  event — excluded from the EMA, counted, and exposed as spent-rate per minute.
- update(t_sec, values) -> dict (see below), reset() clears all state.

The returned per-resource record exposes: the raw value, the EMA income rate
(res/sec), rate*60 (res/min), spent events / minute, the running spend count,
and the pre-formatted display string honouring rate_unit / decimals /
show_plus_sign / show_zero / zero_epsilon.
"""

import math
from dataclasses import asdict
from typing import Dict, Optional

from config import CONFIG


class RateEngine:
    def __init__(self, config=None):
        self.config = config if config is not None else CONFIG
        self.reset()

    def reset(self):
        """Drop all per-resource state (fresh session)."""
        self._ema: Dict[str, float] = {}            # res -> income rate (res/sec)
        self._t_last: Dict[str, float] = {}         # res -> last accepted t_sec
        self._v_last: Dict[str, float] = {}         # res -> value at _t_last
        self._spent: Dict[str, float] = {}          # res -> spend events total
        self._window_start: Dict[str, float] = {}   # res -> t of first spend kept
        self._t_spent: Dict[str, float] = {}        # res -> last spend t_sec
        self._seen: Dict[str, bool] = {}            # res -> has first sample?
        self._raw_t = 0.0

    # ------------------------------------------------------------------ #
    # core: per-resource EMA over real elapsed seconds                    #
    # ------------------------------------------------------------------ #
    def update(self, t_sec: float, values: Dict[str, float]) -> dict:
        """Feed one parsed sample. values = {resource: amount}. Returns a
        per-resource record dict (also kept as .last), see RESULT_KEYS."""
        self._raw_t = t_sec
        result: dict = {"t": t_sec, "resources": {}}
        for res, value in values.items():
            result["resources"][res] = self._update_one(res, t_sec, float(value))
        self.last = result
        return result

    def _update_one(self, res: str, t_sec: float, value: float) -> dict:
        if not self._seen.get(res, False):
            # first sample: seed the baseline, no rate yet (no dt available)
            self._seen[res] = True
            self._ema[res] = 0.0
            self._t_last[res] = t_sec
            self._v_last[res] = value
            self._spent[res] = 0.0
            self._t_spent[res] = None
            self._window_start[res] = t_sec
            return self._make_record(res, value)

        dt = t_sec - self._t_last[res]
        if dt <= 0.0:
            return self._make_record(res, value)  # stale/bad clock — keep last rate
        if dt < self.config.min_dt_sec:
            return self._make_record(res, value)  # dedupe/jitter guard — ignore

        delta = value - self._v_last[res]
        instant = delta / dt  # res/sec
        ema = self._ema[res]

        # spend rule: a DROP bigger than spend_ratio * current EMA is a SPEND.
        if instant < 0.0 and abs(instant) > abs(ema) * self.config.spend_ratio:
            self._spent[res] += 1.0
            self._t_spent[res] = t_sec
            if self._window_start[res] is None:
                self._window_start[res] = t_sec
            # do NOT fold into the EMA — the rate is unchanged by the spend
        else:
            alpha = 1.0 - math.exp(-dt / self.config.smoothing_tau_sec)
            # clamp alpha to (0,1) for numeric safety
            alpha = max(0.0, min(1.0, alpha))
            self._ema[res] = instant * alpha + ema * (1.0 - alpha)

        self._t_last[res] = t_sec
        self._v_last[res] = value
        return self._make_record(res, value)

    # ------------------------------------------------------------------ #
    # output record: raw value + rates + display string                  #
    # ------------------------------------------------------------------ #
    RESULT_KEYS = ("value", "rate", "rate_min", "spent_min", "spend_count",
                   "formatted", "formatted_min")

    def _make_record(self, res: str, value: float) -> dict:
        rate = self._ema.get(res, 0.0)
        rate_min = rate * 60.0
        spent_count = self._spent.get(res, 0.0)
        spent_min = self._spent_per_min(res)
        return {
            "value": value,
            "rate": rate,
            "rate_min": rate_min,
            "spent_min": spent_min,
            "spend_count": spent_count,
            "formatted": format_rate(rate),
            "formatted_min": format_rate(rate, unit="min"),
        }

    def _spent_per_min(self, res: str) -> float:
        """Spend events/min: windowed count since the first kept spend."""
        n = self._spent.get(res, 0.0)
        start = self._window_start.get(res)
        if start is None:
            return 0.0
        span_sec = max(self._raw_t - start, 1.0)
        return n * 60.0 / span_sec

    # ------------------------------------------------------------------ #
    # accessors                                                          #
    # ------------------------------------------------------------------ #
    @property
    def last_t(self) -> float:
        return self._raw_t

    @property
    def raw(self) -> dict:
        return self.last or {}

    def rate(self, res: str, unit: str = "sec") -> float:
        """EMA income rate for a resource; 'min' returns res/min."""
        r = self._ema.get(res, 0.0)
        return r * 60.0 if unit == "min" else r

    def spent(self, res: str) -> float:
        """Total spend events for a resource."""
        return self._spent.get(res, 0.0)

    def display(self, res: str) -> str:
        """Human string for a resource's CURRENT rate honouring display
        settings (single-value helper for status bars)."""
        return format_rate(self.rate(res))


# Also expose each resource's dict helper, mirroring config's vocabulary.
def format_rate(rate: float, unit: str = None, config=None) -> str:
    """Format a rate (res/sec unless unit='min') following the display rules:
    - rate_unit (config.rate_unit) selects sec|min base
    - decimals places, show_plus_sign on positive, show_zero filtering
      (|rate| < zero_epsilon renders '').
    """
    cfg = config if config is not None else CONFIG
    if unit is None:
        unit = cfg.rate_unit
    if abs(rate) < cfg.zero_epsilon and not cfg.show_zero:
        return ""
    rate_val = rate
    if unit == "min":
        rate_val = rate * 60.0
    s = f"{rate_val:.{cfg.decimals}f}"
    if cfg.show_plus_sign and rate_val > 0:
        s = "+" + s
    return s + "/" + unit


def config_as_dict(config=None) -> dict:
    """The active Config flattened for display/debug (GUI status)."""
    return asdict(config if config is not None else CONFIG)