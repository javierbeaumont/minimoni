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
Load a real minimoni TOML config and map it to /api/current fields.

Only the [dashboard] presentation settings are used; unset keys fall back to
the defaults from src/config.c. Requires Python 3.11+ (tomllib).
"""

from logging import getLogger
from os.path import abspath, dirname, join
from sys import exit

from tomllib import TOMLDecodeError, load as toml_load

from mock_data import JSON

log = getLogger("minimoni-dev")

DEFAULT_CONFIG = join(dirname(abspath(__file__)), "..", "..", "config.example.toml")

# Defaults mirror config_defaults() in src/config.c.
CONFIG_DEFAULTS: JSON = {
    "title": "minimoni",
    "theme": "auto",
    "show_footer": True,
    "memory_card_unit": "%",
    "memory_chart_unit": "mb",
    "disk_card_unit": "%",
    "disk_chart_unit": "gb",
    "temp_card_unit": "c",
    "temp_chart_unit": "c",
    "cpu_load_card_unit": "abs",
    "cpu_load_chart_unit": "abs",
    "net_card_unit": "mb",
    "net_chart_unit": "mb",
    "uptime_unit": "auto",
    "ranges": ["1d", "7d", "30d", "90d"],
}


def dashboard_temp_critical_fallback(d: JSON) -> float:
    # Fallback 100% reference for temp percent mode when sysfs has no critical
    # trip point (config.c default 85).
    try:
        v = float(d.get("temp_critical_fallback", 85.0))
        return v if v > 0 else 85.0
    except (TypeError, ValueError):
        return 85.0


def load_dashboard_config(path: str) -> JSON:
    try:
        with open(path, "rb") as f:
            cfg = toml_load(f)
    except FileNotFoundError:
        log.error("config not found: %s", path)
        exit(1)
    except TOMLDecodeError as e:
        log.error("invalid TOML in %s: %s", path, e)
        exit(1)
    return cfg.get("dashboard", {})


def config_fields(d: JSON) -> JSON:
    # Mirrors src/http.c /api/current: config keys -> response fields, including
    # the memory_* -> mem_* rename. charts/cards absent => null (show all).
    def g(key: str) -> object:
        return d.get(key, CONFIG_DEFAULTS[key])

    return {
        "mem_card_unit": g("memory_card_unit"),
        "mem_chart_unit": g("memory_chart_unit"),
        "disk_card_unit": g("disk_card_unit"),
        "disk_chart_unit": g("disk_chart_unit"),
        "temp_card_unit": g("temp_card_unit"),
        "temp_chart_unit": g("temp_chart_unit"),
        "net_card_unit": g("net_card_unit"),
        "net_chart_unit": g("net_chart_unit"),
        "cpu_load_card_unit": g("cpu_load_card_unit"),
        "cpu_load_chart_unit": g("cpu_load_chart_unit"),
        "title": g("title"),
        "theme": g("theme"),
        "show_footer": g("show_footer"),
        "uptime_unit": g("uptime_unit"),
        "ranges": g("ranges"),
        "charts": d.get("charts"),
        "cards": d.get("cards"),
    }
