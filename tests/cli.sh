#!/bin/sh
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

# CLI integration smoke test: exercises the built `minimoni` binary across every
# subcommand path (exit codes + key output) and the serve daemon (bind, health,
# clean SIGTERM shutdown). Run via `make test-cli`, which builds the release
# binary in Docker and then runs this script against it.
#
# Scope: this is a SMOKE / contract harness, not a behaviour suite. Per CLI
# path, assert only that it exists, exits with the right code, and emits its
# key output line: the happy path plus at most one representative error.
# Behavioural depth (branch combinatorics, edge cases) belongs in the C unit
# suites, which are faster and need no built binary. For example, db_cmd_info's
# foreign-DB / no-metrics / v0.1 branches live in tests/unit-db_cmd.c, not
# here. If this file passes ~30 checks, stop and re-evaluate: either scope crept
# down from a unit test, or the CLI genuinely outgrew a shell smoke test (in
# which case reconsider its structure).
set -u

BIN=./minimoni
PORT=18099
pass=0
fail=0

check_rc() { # DESC EXPECTED ACTUAL
    if [ "$2" -eq "$3" ]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$1"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s (rc=%s, want %s)\n' "$1" "$3" "$2"
    fi
}

check_has() { # DESC HAYSTACK NEEDLE
    if printf '%s' "$2" | grep -qF -- "$3"; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$1"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s (missing: %s)\n' "$1" "$3"
    fi
}

check_lacks() { # DESC HAYSTACK NEEDLE
    if printf '%s' "$2" | grep -qF -- "$3"; then
        fail=$((fail + 1))
        printf '  FAIL  %s (found: %s)\n' "$1" "$3"
    else
        pass=$((pass + 1))
        printf '  ok    %s\n' "$1"
    fi
}

expect_rc() { # EXPECTED DESC CMD...
    exp=$1
    desc=$2
    shift 2
    "$@" >/dev/null 2>&1
    rc=$?
    check_rc "$desc" "$exp" "$rc"
}

[ -x "$BIN" ] || {
    echo "cli.sh: $BIN not built (run 'make release' first)" >&2
    exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cfg="$work/config.toml"
cat >"$cfg" <<EOF
[collect]
db = "$work/metrics.db"
interval = 60
disk_path = "/"
[server]
listen = "127.0.0.1:$PORT"
EOF

echo "CLI integration tests:"

# --- meta ---
v=$("$BIN" --version 2>/dev/null)
rc=$?
check_rc "--version exits 0" 0 "$rc"
check_has "--version prints version" "$v" "minimoni "
expect_rc 0 "--help exits 0" "$BIN" --help
expect_rc 0 "-h exits 0" "$BIN" -h
expect_rc 1 "no args exits 1" "$BIN"
expect_rc 1 "unknown subcommand exits 1" "$BIN" bogus

# --- collect ---
expect_rc 0 "collect exits 0" "$BIN" collect --config "$cfg"
if [ -s "$work/metrics.db" ]; then
    pass=$((pass + 1))
    printf '  ok    %s\n' "collect creates the database"
else
    fail=$((fail + 1))
    printf '  FAIL  %s\n' "collect creates the database"
fi
expect_rc 1 "collect with missing config exits 1" "$BIN" collect --config "$work/none.toml"

# --- db info ---
info=$("$BIN" db info "$work/metrics.db" 2>/dev/null)
rc=$?
check_rc "db info exits 0" 0 "$rc"
check_has "db info prints format" "$info" "moni v"
check_has "db info prints row count" "$info" "Total rows"
expect_rc 1 "db info missing file exits 1" "$BIN" db info "$work/none.db"
expect_rc 1 "db unknown action exits 1" "$BIN" db bogus "$work/metrics.db"
expect_rc 1 "db without action exits 1" "$BIN" db

# --- serve ---
"$BIN" serve --config "$cfg" >/dev/null 2>&1 &
sv=$!
sleep 1
health=$(wget -qO- "http://127.0.0.1:$PORT/api/health" 2>/dev/null)
check_has "serve answers /api/health" "$health" '"status":"ok"'
# /api/current + /api/metrics smoke: confirm the binary wires both serializers
# over HTTP. Exact threshold values, card/chart gating and null handling are
# covered exhaustively by unit-json.c, so one assertion per endpoint suffices.
current=$(wget -qO- "http://127.0.0.1:$PORT/api/current" 2>/dev/null)
check_has "serve /api/current serializes a snapshot" "$current" '"thresh_cpu":[70,90]'
metrics=$(wget -qO- "http://127.0.0.1:$PORT/api/metrics?range=1d" 2>/dev/null)
check_has "serve /api/metrics serializes points" "$metrics" '"l1":'
# A dashboard script missing its bundle.sh marker ships as a dangling <script src=>
# that breaks ONLY the embedded copy (the devserver serves files and hides it).
home=$(wget -qO- "http://127.0.0.1:$PORT/" 2>/dev/null)
# minify strips attribute quotes, so match a bare tag rather than id="...".
check_has "serve / serves the dashboard" "$home" '<canvas'
check_lacks "dashboard bundle fully inlined" "$home" 'script src='
# /stream is the only SSE path: read the first frame, then drop the connection.
frame=$(timeout 3 wget -qO- "http://127.0.0.1:$PORT/stream" 2>/dev/null | head -c 40)
check_has "serve /stream pushes an SSE frame" "$frame" 'data: {'
# Query-parameter fallbacks: the server clamps instead of erroring.
bad=$(wget -qO- "http://127.0.0.1:$PORT/api/metrics?range=bogus" 2>/dev/null)
check_has "serve /api/metrics falls back on a bad range" "$bad" '"range":"1d"'
big=$(wget -qO- "http://127.0.0.1:$PORT/api/metrics?range=1d&points=99999" 2>/dev/null)
check_has "serve /api/metrics survives an oversized points" "$big" '"points":'
code=$(wget -qO- -S "http://127.0.0.1:$PORT/nope" 2>&1 | grep -m1 HTTP)
check_has "serve 404s an unknown path" "$code" '404'
kill -TERM "$sv" 2>/dev/null
wait "$sv"
rc=$?
check_rc "serve shuts down cleanly on SIGTERM" 0 "$rc"
expect_rc 1 "serve with missing config exits 1" "$BIN" serve --config "$work/none.toml"

printf '\n  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
