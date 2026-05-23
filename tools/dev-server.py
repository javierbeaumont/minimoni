#!/usr/bin/env python3
"""
Mock HTTP server for iterating on dashboard/index.html without compiling.
Usage: python3 tools/dev-server.py [port]
Serves dashboard/index.html at / and fake data at /api/current, /api/metrics, /stream.
"""

import json
import math
import os
import random
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9090
DASHBOARD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'dashboard')
DASHBOARD = os.path.join(DASHBOARD_DIR, 'index.html')

STATIC_FILES = {
    'app.js':      'text/javascript',
    'style.css':   'text/css',
    'favicon.svg': 'image/svg+xml',
}

def _git_version():
    try:
        return subprocess.check_output(
            ['git', 'describe', '--tags', '--always'],
            cwd=os.path.dirname(__file__), stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return 'unknown'

VERSION = _git_version()

# --- mock data -----------------------------------------------------------------

BASE_TS = int(time.time()) - 86400  # 24 h ago


def _wave(i, n, base, amp, noise=0.0):
    x = 2 * math.pi * i / max(n - 1, 1)
    v = base + amp * math.sin(x) + noise * (random.random() - 0.5)
    return max(0.0, v)


def make_points(n=300):
    pts = []
    step = 86400 // n
    for i in range(n):
        t = BASE_TS + i * step
        cpu_u = _wave(i, n, 12.0, 10.0, 4.0)
        cpu_s = _wave(i, n, 3.0, 2.0, 1.0)
        l1 = _wave(i, n, 0.6, 0.5, 0.15)
        pts.append({
            "t":   t,
            "l1":  round(l1, 2),
            "l5":  round(_wave(i, n, 0.5, 0.3, 0.05), 2),
            "l15": round(_wave(i, n, 0.4, 0.2, 0.02), 2),
            "cu":  round(cpu_u, 1),
            "cs":  round(cpu_s, 1),
            "mu":  round(_wave(i, n, 820.0, 60.0, 10.0), 0),
            "ma":  round(_wave(i, n, 1100.0, 60.0, 10.0), 0),
            "mt":  1959.0,
            "mp":  round(_wave(i, n, 41.0, 3.0, 0.5), 1),
            "du":  round(7.4 + i * 0.0001, 2),
            "df":  round(90.5 - i * 0.0001, 2),
            "dt":  97.9,
            "dp":  round(7.6 + i * 0.0001, 1),
            "tp":  round(_wave(i, n, 52.0, 8.0, 1.0), 1),
            "nr":  round(_wave(i, n, 1.2, 1.0, 0.3), 2),
            "nt":  round(_wave(i, n, 0.3, 0.2, 0.1), 2),
            "up":  72000.0 + i * step,
        })
    return pts


POINTS = make_points(300)

CURRENT = {
    "timestamp":          time.strftime("%Y-%m-%d %H:%M:%S"),
    "load_1m":            0.38,
    "load_5m":            0.31,
    "load_15m":           0.22,
    "cpu_user_percent":   14.2,
    "cpu_system_percent": 2.8,
    "cpu_idle_percent":   83.0,
    "mem_used":           832.0,
    "mem_available":      1127.0,
    "mem_total":          1959.0,
    "mem_percent":        42.5,
    "disk_used":          7.4,
    "disk_total":         97.9,
    "disk_free":          90.5,
    "disk_percent":       7.6,
    "temp":               51.3,
    "temp_critical":      105.0,
    "net_rx":             1.14,
    "net_tx":             0.28,
    "uptime_seconds":     72840.0,
    "mem_card_unit":       "%",
    "mem_chart_unit":      "mb",
    "disk_card_unit":      "%",
    "disk_chart_unit":     "gb",
    "temp_card_unit":      "c",
    "temp_chart_unit":     "c",
    "net_card_unit":       "mb",
    "net_chart_unit":      "mb",
    "cpu_load_card_unit":  "abs",
    "cpu_load_chart_unit": "abs",
    "title":               "minimoni",
    "version":             VERSION,
    "theme":               "auto",
    "show_footer":         True,
    "uptime_unit":         "auto",
    "ranges":              ["1d", "7d", "30d", "90d"],
    "charts":              None,
    "cards":               None,
}

# --- handler -------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silent

    def _send(self, code, ctype, body):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path in ('/', '/index.html'):
            with open(DASHBOARD, 'rb') as f:
                self._send(200, 'text/html; charset=utf-8', f.read())

        elif path == '/api/current':
            self._send(200, 'application/json', json.dumps(CURRENT))

        elif path == '/api/metrics':
            qs = parse_qs(parsed.query)
            r = qs.get('range', ['1d'])[0]
            # Serve a slice of POINTS based on range
            n_map = {'1d': 300, '7d': 300, '30d': 300, '90d': 300}
            n = n_map.get(r, 300)
            pts = POINTS[-n:]
            self._send(200, 'application/json',
                       json.dumps({"range": r, "points": pts}))

        elif path == '/stream':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            try:
                for _ in range(3):
                    data = json.dumps(CURRENT)
                    self.wfile.write(f'data: {data}\n\n'.encode())
                    self.wfile.flush()
                    time.sleep(5)
            except (BrokenPipeError, ConnectionResetError):
                pass

        else:
            # Serve static files from dashboard/ — strict allowlist of known
            # filenames. Defence in depth: after looking up the filename in
            # the allowlist, realpath() resolves any symlink trickery and
            # startswith() confirms the result stays within DASHBOARD_DIR.
            # This is the path-injection sanitiser CodeQL recognises.
            filename = os.path.basename(path)
            if filename in STATIC_FILES:
                ctype = STATIC_FILES[filename]
                fpath = os.path.realpath(os.path.join(DASHBOARD_DIR, filename))
                base = os.path.realpath(DASHBOARD_DIR)
                if fpath.startswith(base + os.sep) and os.path.isfile(fpath):
                    with open(fpath, 'rb') as f:
                        self._send(200, ctype, f.read())
                    return
            self._send(404, 'text/plain', 'not found')


# --- main ----------------------------------------------------------------------

if __name__ == '__main__':
    server = HTTPServer(('127.0.0.1', PORT), Handler)
    print(f'minimoni dev server on http://localhost:{PORT}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
