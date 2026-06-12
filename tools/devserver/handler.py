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
HTTP request handler for the minimoni dev server.

A handler is bound to an AppState (version, real config fields, temp_max,
scenario) via make_handler(), so there is no module-global state. Raw mock
metrics are converted to the configured units by units.py.
"""

from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler
from json import dumps
from logging import getLogger
from os import sep
from os.path import abspath, basename, dirname, isfile, join, realpath
from time import sleep
from urllib.parse import parse_qs, urlparse

from mock_data import JSON, clamp_points, current_snapshot, make_points
from units import to_current, to_point

log = getLogger("minimoni-dev")

REPO_ROOT = join(dirname(abspath(__file__)), "..", "..")
DASHBOARD_DIR = join(REPO_ROOT, "dashboard")
DASHBOARD = join(DASHBOARD_DIR, "index.html")

STATIC_FILES: dict[str, str] = {
    "app.js": "text/javascript",
    "style.css": "text/css",
    "favicon.svg": "image/svg+xml",
}


@dataclass
class AppState:
    version: str
    config_fields: JSON
    temp_max: float
    scenario: str


def make_handler(state: AppState) -> type[BaseHTTPRequestHandler]:
    """Build a request handler bound to the given app state."""

    def current() -> JSON:
        # converted mocked metrics + version + real config fields
        raw = current_snapshot(state.scenario)
        return {
            **to_current(raw, state.config_fields, state.temp_max),
            "version": state.version,
            **state.config_fields,
        }

    def metrics(range_value: str, n_points: int) -> JSON:
        points = [
            to_point(p, state.config_fields, state.temp_max)
            for p in make_points(range_value, state.scenario, n_points)
        ]
        return {"range": range_value, "points": points}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args: object) -> None:
            log.debug("%s %s", self.address_string(), fmt % args)

        def _send(self, code: int, ctype: str, body: bytes | str) -> None:
            if isinstance(body, str):
                body = body.encode()
            try:
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # client went away mid-response

        def do_GET(self) -> None:
            try:
                self._route()
            except Exception:
                log.exception("error handling %s", self.path)
                self._send(500, "text/plain", "internal error")

        def _route(self) -> None:
            parsed = urlparse(self.path)
            path = parsed.path

            if path in ("/", "/index.html"):
                self._serve_dashboard()
            elif path == "/api/current":
                self._send(200, "application/json", dumps(current()))
            elif path == "/api/metrics":
                q = parse_qs(parsed.query)
                r = q.get("range", ["1d"])[0]
                n_points = clamp_points(q.get("points", [""])[0])
                self._send(200, "application/json", dumps(metrics(r, n_points)))
            elif path == "/stream":
                self._stream()
            else:
                self._serve_static(path)

        def _serve_dashboard(self) -> None:
            try:
                with open(DASHBOARD, "rb") as f:
                    self._send(200, "text/html; charset=utf-8", f.read())
            except FileNotFoundError:
                log.error("dashboard not found: %s", DASHBOARD)
                self._send(404, "text/plain", "dashboard not found")

        def _stream(self) -> None:
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                for _ in range(3):
                    self.wfile.write(f"data: {dumps(current())}\n\n".encode())
                    self.wfile.flush()
                    sleep(5)
            except (BrokenPipeError, ConnectionResetError):
                pass  # client closed the stream

        def _serve_static(self, path: str) -> None:
            # Serve static files from dashboard/: strict allowlist of known
            # filenames. Defence in depth: after looking up the filename in
            # the allowlist, realpath() resolves any symlink trickery and
            # startswith() confirms the result stays within DASHBOARD_DIR.
            # This is the path-injection sanitiser CodeQL recognises.
            filename = basename(path)
            if filename in STATIC_FILES:
                fpath = realpath(join(DASHBOARD_DIR, filename))
                base = realpath(DASHBOARD_DIR)
                if fpath.startswith(base + sep) and isfile(fpath):
                    with open(fpath, "rb") as f:
                        self._send(200, STATIC_FILES[filename], f.read())
                    return
            self._send(404, "text/plain", "not found")

    return Handler
