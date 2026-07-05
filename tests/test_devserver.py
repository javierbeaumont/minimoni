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
Unit tests for the dev-server pure functions. Zero-dependency, no framework
(stdlib assert + a tiny runner). One suite for the whole devserver package;
add cases here as more pure helpers appear. Run via `make test`.
"""

from os.path import abspath, dirname, join
from sys import exit, path

path.insert(0, join(dirname(abspath(__file__)), "..", "tools", "devserver"))

from config import config_fields  # noqa: E402
from mock_data import _range_seconds, clamp_points, current_snapshot  # noqa: E402
from units import temp_convert, temp_ref, to_current, to_point  # noqa: E402


# --- clamp_points: the /api/metrics points hint (mirror of the C server) ---


def test_clamp_points_valid():
    assert clamp_points("480") == 480


def test_clamp_points_at_cap():
    assert clamp_points("1440") == 1440


def test_clamp_points_above_cap():
    assert clamp_points("99999") == 1440


def test_clamp_points_zero_defaults():
    assert clamp_points("0") == 240


def test_clamp_points_negative_defaults():
    assert clamp_points("-5") == 240


def test_clamp_points_empty_defaults():
    assert clamp_points("") == 240


def test_clamp_points_non_numeric_defaults():
    assert clamp_points("abc") == 240


# --- _range_seconds: range string -> seconds (m, h, d) ---


def test_range_seconds_minutes():
    assert _range_seconds("30m") == 1800


def test_range_seconds_hours():
    assert _range_seconds("12h") == 43200


def test_range_seconds_days():
    assert _range_seconds("7d") == 604800


# --- temp_ref / temp_convert: percent ref = sysfs critical, else fallback ---


def test_temp_ref_uses_critical():
    assert temp_ref(105.0, 85.0) == 105.0


def test_temp_ref_falls_back_when_none():
    assert temp_ref(None, 85.0) == 85.0


def test_temp_convert_percent_at_ref():
    assert temp_convert(85.0, "%", 85.0) == 100.0


def test_temp_convert_percent_half():
    assert temp_convert(42.5, "%", 85.0) == 50.0


def test_temp_convert_fahrenheit():
    assert temp_convert(100.0, "f", 85.0) == 212.0


def test_temp_convert_celsius():
    assert temp_convert(50.0, "c", 85.0) == 50.0


def test_range_seconds_invalid_defaults_one_day():
    assert _range_seconds("bogus") == 86400


# --- to_current: server-computed thresholds + card field gating ---


def test_to_current_emits_thresholds():
    out = to_current(current_snapshot("normal"), config_fields({}), 85.0)
    assert out["thresh_cpu"] == [70.0, 90.0]
    assert out["thresh_mem"] == [70.0, 90.0]
    assert out["thresh_disk"] == [80.0, 90.0]
    assert "thresh_load" in out and len(out["thresh_load"]) == 2


def test_to_current_gates_excluded_cards():
    f = config_fields({"cards": ["cpu_load"]})
    out = to_current(current_snapshot("normal"), f, 85.0)
    assert "load_1m" in out and "thresh_load" in out
    assert "mem_percent" not in out and "thresh_mem" not in out
    assert "net_rx" not in out


# --- to_point: chart-unit short keys + temp gated by charts ---


def _point(scenario="normal"):
    p = current_snapshot(scenario)
    p["t"] = 1700000000
    return p


def test_to_point_emits_short_keys():
    out = to_point(_point(), config_fields({}), 85.0)
    for k in ("t", "l1", "cu", "mp", "dp", "nr", "up"):
        assert k in out


def test_to_point_temp_gated_by_charts():
    assert "tp" in to_point(_point(), config_fields({}), 85.0)
    assert "tp" not in to_point(_point(), config_fields({"charts": ["cpu_load"]}), 85.0)


def main():
    tests = sorted(
        (name, fn)
        for name, fn in globals().items()
        if name.startswith("test_") and callable(fn)
    )
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("  %-45s ok" % name)
        except AssertionError:
            failed += 1
            print("  %-45s FAIL" % name)
    print("\n  %d/%d tests passed" % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == "__main__":
    exit(main())
