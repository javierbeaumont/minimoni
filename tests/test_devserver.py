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

import os
import sys

sys.path.insert(
    0,
    os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "tools", "devserver"
    ),
)

from mock_data import _range_seconds, clamp_points  # noqa: E402


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


def test_range_seconds_invalid_defaults_one_day():
    assert _range_seconds("bogus") == 86400


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
    sys.exit(main())
