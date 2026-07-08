#!/usr/bin/env python3
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
Mock HTTP server for iterating on dashboard/index.html without compiling.
Usage: python3 tools/devserver/ [port] [config] [--scenario S] [--flaky] [-v]
Metric values are mocked (mock_data.py); presentation settings come from a real
minimoni config (config.py); the HTTP serving lives in handler.py.
Requires Python 3.11+ (uses tomllib).
Serves dashboard/index.html at / and data at /api/current, /api/metrics, /stream.
"""

from argparse import ArgumentParser
from http.server import ThreadingHTTPServer
from logging import DEBUG, INFO, basicConfig, getLogger
from os.path import abspath, dirname
from subprocess import DEVNULL, check_output
from sys import exit, stderr, version_info

if version_info < (3, 11):
    print("minimoni dev-server requires Python 3.11+ (uses tomllib)", file=stderr)
    exit(1)

from config import (
    DEFAULT_CONFIG,
    config_fields,
    dashboard_temp_critical_fallback,
    load_dashboard_config,
)
from handler import AppState, make_handler

log = getLogger("minimoni-dev")


def git_version() -> str:
    try:
        return (
            check_output(
                ["git", "describe", "--tags", "--always"],
                cwd=dirname(abspath(__file__)),
                stderr=DEVNULL,
            )
            .decode()
            .strip()
        )
    except Exception:
        return "unknown"


def main() -> None:
    parser = ArgumentParser(
        description="minimoni dashboard dev server (mock data, real config)"
    )
    parser.add_argument(
        "port", nargs="?", type=int, default=9090, help="listen port (default: 9090)"
    )
    parser.add_argument(
        "config",
        nargs="?",
        default=DEFAULT_CONFIG,
        help="minimoni TOML config (default: config.example.toml)",
    )
    parser.add_argument(
        "--scenario",
        choices=["normal", "warn", "critical", "cycle"],
        default="cycle",
        help="card stress level (default: cycle, sweeps good->critical over time)",
    )
    parser.add_argument(
        "--flaky",
        action="store_true",
        help="drop every other /stream connection, to exercise the SSE indicator",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="log every request"
    )
    args = parser.parse_args()

    basicConfig(
        level=DEBUG if args.verbose else INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        stream=stderr,
    )

    dashboard = load_dashboard_config(args.config)
    state = AppState(
        version=git_version(),
        config_fields=config_fields(dashboard),
        temp_critical_fallback=dashboard_temp_critical_fallback(dashboard),
        scenario=args.scenario,
        flaky=args.flaky,
    )
    log.info("config loaded from %s (scenario=%s)", args.config, args.scenario)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state))
    log.info("minimoni dev server on http://localhost:%d", args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
