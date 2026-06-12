# minimoni - zero-dependency system monitoring
# Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

"""
Mock metric data for the minimoni dev server.

Produces fake metrics in the SAME base units the real collector stores (load
average, percent, MB, GB, Celsius, bytes/sec), so units.py can apply the
configured unit conversions exactly like src/http.c. Values can reach the
warning and critical bands. No real system is read.
"""

from math import cos, pi, sin
from random import random
from time import strftime, time

JSON = dict[str, object]

_DAY_SECONDS = 86400
_CYCLE_PERIOD = 60  # seconds for one good -> critical -> good sweep of the cards
# Stress level (0 = healthy, 1 = critical) for each pinned scenario.
_SCENARIO_STRESS = {"normal": 0.10, "warn": 0.85, "critical": 0.97}

_MEM_TOTAL_MB = 1959.0
_DISK_TOTAL_GB = 97.9
_TEMP_CRITICAL_C = 105.0


def _range_seconds(value: str) -> int:
    """Parse a range like '7d', '12h' or '30m' into seconds; default 1 day."""
    units = {"d": _DAY_SECONDS, "h": 3600, "m": 60}
    try:
        return int(value[:-1]) * units[value[-1]]
    except (ValueError, KeyError, IndexError):
        return _DAY_SECONDS


def clamp_points(raw: str, cap: int = 1440, default: int = 240) -> int:
    """Parse a points query value: a positive int capped at `cap`, else `default`
    when missing, non-numeric, or non-positive. Mirrors the server contract."""
    return min(int(raw), cap) if raw.isdigit() and int(raw) > 0 else default


def _card_stress(scenario: str) -> float:
    """Stress for the live cards: 'cycle' sweeps over wall-clock time."""
    if scenario == "cycle":
        return (sin(2 * pi * (time() % _CYCLE_PERIOD) / _CYCLE_PERIOD) + 1) / 2
    return _SCENARIO_STRESS.get(scenario, _SCENARIO_STRESS["normal"])


def _series_stress(scenario: str, i: int, n: int) -> float:
    """Stress for chart point i: 'cycle' sweeps low->critical->low across the chart."""
    if scenario == "cycle":
        return (1 - cos(2 * pi * i / max(n - 1, 1))) / 2
    return _SCENARIO_STRESS.get(scenario, _SCENARIO_STRESS["normal"])


def _band(s: float, ripple: float, lo: float, hi: float, amp: float = 0.0) -> float:
    """Value at stress s in [lo, hi], plus a small ripple for texture, clamped >= 0."""
    return max(0.0, lo + s * (hi - lo) + amp * ripple)


def _metrics(s: float, ripple: float) -> JSON:
    """Base-unit metric values at stress s (0 healthy .. 1 critical)."""
    mem_percent = min(99.0, _band(s, ripple, 38.0, 95.0, 2.0))
    disk_percent = min(99.0, _band(s, ripple, 8.0, 95.0, 1.0))
    load_1m = _band(s, ripple, 0.35, 4.6, 0.2)
    cpu_user = min(99.0, _band(s, ripple, 12.0, 95.0, 4.0))
    cpu_system = _band(s, ripple, 2.0, 4.0, 0.5)
    return {
        "load_1m": load_1m,
        "load_5m": load_1m * 0.85,
        "load_15m": load_1m * 0.70,
        "cpu_user": cpu_user,
        "cpu_system": cpu_system,
        "cpu_idle": max(0.0, 100.0 - cpu_user - cpu_system),
        "mem_used_mb": _MEM_TOTAL_MB * mem_percent / 100.0,
        "mem_avail_mb": _MEM_TOTAL_MB * (100.0 - mem_percent) / 100.0,
        "mem_total_mb": _MEM_TOTAL_MB,
        "mem_percent": mem_percent,
        "disk_used_gb": _DISK_TOTAL_GB * disk_percent / 100.0,
        "disk_total_gb": _DISK_TOTAL_GB,
        "disk_free_gb": _DISK_TOTAL_GB * (100.0 - disk_percent) / 100.0,
        "disk_percent": disk_percent,
        "temp_c": _band(s, ripple, 48.0, 108.0, 3.0),
        "net_rx_bps": _band(s, ripple, 1.26e6, 8.4e6, 2e5),
        "net_tx_bps": _band(s, ripple, 0.31e6, 3.1e6, 1e5),
        "uptime": 72840.0,
    }


def current_snapshot(scenario: str = "cycle") -> JSON:
    """Live-ish base-unit snapshot; stress reaches warn/critical (see units.py).

    scenario: 'normal' | 'warn' | 'critical' | 'cycle' (default sweeps over time).
    """
    m = _metrics(_card_stress(scenario), random() - 0.5)
    m["timestamp"] = strftime("%Y-%m-%d %H:%M:%S")
    m["temp_critical_c"] = _TEMP_CRITICAL_C
    return m


def make_points(
    range_value: str = "1d", scenario: str = "cycle", n: int = 300
) -> list[JSON]:
    """n base-unit metric points spanning the range, scaled by the scenario stress."""
    step = max(_range_seconds(range_value) // n, 1)
    start = int(time()) - step * (n - 1)
    points: list[JSON] = []
    for i in range(n):
        t = start + i * step
        ripple = sin(2 * pi * (t % _DAY_SECONDS) / _DAY_SECONDS)
        m = _metrics(_series_stress(scenario, i, n), ripple)
        m["t"] = t
        points.append(m)
    return points
