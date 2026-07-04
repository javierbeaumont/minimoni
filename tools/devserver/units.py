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
Unit conversions for the minimoni dev server, mirroring src/http.c exactly.

Raw base-unit metrics (from mock_data) are converted to the configured display
units: /api/current uses the *_card_unit values, /api/metrics uses *_chart_unit.
Memory/disk raw fields are OMITTED when the unit is "%" (as the real server does,
so the dashboard falls back to the percentage), and the load is normalised by
core count when its unit is "%".
"""

from typing import cast

from mock_data import JSON

# The real server reads the core count from sysfs; the dashboard assumes 4 when
# normalising load client-side, so the dev server uses the same assumption.
CORES = 4


def net_convert(bps: float, unit: str) -> float:
    if not unit or unit[0] == "m":
        if len(unit) > 2 and unit[1] == "b" and unit[2] == "p":  # mbps
            return bps * 8.0 / 1e6
        return bps / 1048576.0  # mb
    if unit[0] == "g":
        if len(unit) > 2 and unit[1] == "b" and unit[2] == "p":  # gbps
            return bps * 8.0 / 1e9
        return bps / 1073741824.0  # gb
    if unit[0] == "k":
        if len(unit) > 2 and unit[1] == "b" and unit[2] == "p":  # kbps
            return bps * 8.0 / 1000.0
        return bps / 1024.0  # kb
    return bps / 1048576.0


def mem_convert(mb: float, unit: str) -> float:
    return mb / 1024.0 if unit and unit[0] == "g" else mb


def disk_convert(gb: float, unit: str) -> float:
    return gb / 1024.0 if unit and unit[0] == "t" else gb


def temp_convert(celsius: float, unit: str, ref: float) -> float:
    if not unit:
        return celsius
    if unit[0] == "f":
        return celsius * 9.0 / 5.0 + 32.0
    if unit[0] == "%":
        return celsius * 100.0 / ref if ref > 0 else celsius
    return celsius


def temp_ref(crit: float | None, fallback: float) -> float:
    """100% reference for temp percent mode: the sysfs critical trip point when
    present, else the configured fallback. Mirrors the C server."""
    return crit if crit is not None else fallback


def load_convert(load: float, cores: int, unit: str) -> float:
    if unit and unit[0] == "%" and cores > 0:
        return load * 100.0 / cores
    return load


def to_current(raw: JSON, f: JSON, fallback: float) -> JSON:
    """Serialize a raw snapshot for /api/current using the CARD units."""
    # The metric values are numeric at runtime; the str/int passthroughs
    # (timestamp, uptime) are read from `raw` directly.
    m = cast(dict[str, float], raw)
    lu, mu = str(f["cpu_load_card_unit"]), str(f["mem_card_unit"])
    du, nu, tu = (
        str(f["disk_card_unit"]),
        str(f["net_card_unit"]),
        str(f["temp_card_unit"]),
    )

    out: JSON = {
        "timestamp": raw["timestamp"],
        "load_1m": round(load_convert(m["load_1m"], CORES, lu), 2),
        "load_5m": round(load_convert(m["load_5m"], CORES, lu), 2),
        "load_15m": round(load_convert(m["load_15m"], CORES, lu), 2),
        "cpu_user_percent": round(m["cpu_user"], 1),
        "cpu_system_percent": round(m["cpu_system"], 1),
        "cpu_idle_percent": round(m["cpu_idle"], 1),
    }
    if mu[0] != "%":
        out["mem_used"] = round(mem_convert(m["mem_used_mb"], mu), 2)
        out["mem_available"] = round(mem_convert(m["mem_avail_mb"], mu), 2)
        out["mem_total"] = round(mem_convert(m["mem_total_mb"], mu), 2)
    out["mem_percent"] = round(m["mem_percent"], 1)
    if du[0] != "%":
        out["disk_used"] = round(disk_convert(m["disk_used_gb"], du), 2)
        out["disk_total"] = round(disk_convert(m["disk_total_gb"], du), 2)
        out["disk_free"] = round(disk_convert(m["disk_free_gb"], du), 2)
    out["disk_percent"] = round(m["disk_percent"], 1)

    tc = m.get("temp_c")
    crit = m.get("temp_critical_c")
    ref = temp_ref(crit, fallback)
    out["temp"] = round(temp_convert(tc, tu, ref), 1) if tc is not None else None
    out["temp_critical"] = (
        round(temp_convert(crit, tu, ref), 1) if crit is not None else None
    )

    out["net_rx"] = round(net_convert(m["net_rx_bps"], nu), 2)
    out["net_tx"] = round(net_convert(m["net_tx_bps"], nu), 2)
    out["uptime_seconds"] = raw["uptime"]
    return out


def to_point(raw: JSON, f: JSON, fallback: float) -> JSON:
    """Serialize a raw point for /api/metrics using the CHART units (short keys)."""
    # The metric values are numeric at runtime; the int passthrough (t, uptime)
    # is read from `raw` directly.
    m = cast(dict[str, float], raw)
    lu, mu = str(f["cpu_load_chart_unit"]), str(f["mem_chart_unit"])
    du, nu, tu = (
        str(f["disk_chart_unit"]),
        str(f["net_chart_unit"]),
        str(f["temp_chart_unit"]),
    )

    out: JSON = {
        "t": raw["t"],
        "l1": round(load_convert(m["load_1m"], CORES, lu), 2),
        "l5": round(load_convert(m["load_5m"], CORES, lu), 2),
        "l15": round(load_convert(m["load_15m"], CORES, lu), 2),
        "cu": round(m["cpu_user"], 1),
        "cs": round(m["cpu_system"], 1),
        "ci": round(m["cpu_idle"], 1),
    }
    if mu[0] != "%":
        out["mu"] = round(mem_convert(m["mem_used_mb"], mu), 2)
        out["ma"] = round(mem_convert(m["mem_avail_mb"], mu), 2)
        out["mt"] = round(mem_convert(m["mem_total_mb"], mu), 2)
    out["mp"] = round(m["mem_percent"], 1)
    if du[0] != "%":
        out["du"] = round(disk_convert(m["disk_used_gb"], du), 2)
        out["dt"] = round(disk_convert(m["disk_total_gb"], du), 2)
        out["df"] = round(disk_convert(m["disk_free_gb"], du), 2)
    out["dp"] = round(m["disk_percent"], 1)

    tc = m.get("temp_c")
    ref = temp_ref(m.get("temp_critical_c"), fallback)
    out["tp"] = round(temp_convert(tc, tu, ref), 1) if tc is not None else None

    out["nr"] = round(net_convert(m["net_rx_bps"], nu), 2)
    out["nt"] = round(net_convert(m["net_tx_bps"], nu), 2)
    out["up"] = raw["uptime"]
    return out
