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

# --- units end to end ---
#
# Every accepted value of every *_unit key, against one seeded row, through the
# real binary. Unit tests cover each converter; this covers the wiring from the
# database to the response, which is where a mismatched key or a dropped call
# would hide. The row: 1748/2048 MB memory, 20/100 GB disk, 50 C, 5 MiB/s down
# and 1 MiB/s up, load 2.
#
# Its own database, seeded and nothing else: the magnitude comes from the
# largest value in the window, so rows collected from the host would hand the
# choice to whatever memory the machine happens to have (MB in a small
# container, GB on a 16 GB CI runner).
udb="$work/units.db"
cat >"$work/units-seed.toml" <<EOF
[collect]
db = "$udb"
interval = 60
disk_path = "/"
[server]
listen = "127.0.0.1:$PORT"
EOF
"$BIN" serve --config "$work/units-seed.toml" >/dev/null 2>&1 &
usv=$!
sleep 1
kill -TERM "$usv" 2>/dev/null
wait "$usv" 2>/dev/null
"$BIN" db exec "$udb" "INSERT INTO metrics (timestamp, load_1m, load_5m, \
  load_15m, cpu_user_percent, cpu_system_percent, cpu_idle_percent, mem_total_mb, \
  mem_used_mb, mem_available_mb, mem_percent, disk_total_gb, disk_used_gb, disk_free_gb, \
  disk_percent, temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds, bucket_sec) VALUES \
  (strftime('%Y-%m-%dT%H:%M:%SZ','now'), 2.0, 2.0, 2.0, 30.0, 10.0, 60.0, 2048.0, 1748.0, \
   300.0, 85.35, 100.0, 20.0, 80.0, 20.0, 50.0, 5242880.0, 1048576.0, 90061.0, 0)" >/dev/null 2>&1
check_rc "units: seed row inserted" 0 $?

units_serve() { # CFG_BODY -> sets $current and $metrics
    cat >"$work/u.toml" <<EOF
[collect]
db = "$udb"
interval = 60
disk_path = "/"
[server]
listen = "127.0.0.1:$PORT"
[dashboard]
net_max_speed = 100
$1
EOF
    "$BIN" serve --config "$work/u.toml" >/dev/null 2>&1 &
    usv=$!
    sleep 1
    current=$(wget -qO- "http://127.0.0.1:$PORT/api/current" 2>/dev/null)
    metrics=$(wget -qO- "http://127.0.0.1:$PORT/api/metrics?range=1d" 2>/dev/null)
    kill -TERM "$usv" 2>/dev/null
    wait "$usv" 2>/dev/null
}

# Everything in "%": the absolute fields are omitted and each value is a share of
# its own ceiling. Net: 5242880 B/s over a 100 Mbit link (12500000 B/s) = 41.9%.
units_serve 'memory_card_unit = "%"
disk_card_unit = "%"
temp_card_unit = "%"
cpu_load_card_unit = "%"
net_card_unit = "%"'
check_lacks "units %: memory omits the absolute fields" "$current" '"mem_used"'
check_lacks "units %: disk omits the absolute fields" "$current" '"disk_used"'
check_has "units %: memory is a percentage" "$current" '"mem_percent":85.35'
check_has "units %: net is a share of the link" "$current" '"net_rx":41.9'
check_has "units %: load is normalised by cores" "$current" '"thresh_load":[70,90]'

# Everything absolute: values travel in their base unit, untouched, and the
# envelope carries the magnitude the dashboard should read them in.
units_serve 'memory_card_unit = "auto"
memory_chart_unit = "auto"
disk_card_unit = "auto"
disk_chart_unit = "auto"
temp_card_unit = "c"
cpu_load_card_unit = "abs"
net_card_unit = "bytes"
net_chart_unit = "bytes"'
check_has "units auto: memory in base MB" "$current" '"mem_used":1748'
check_has "units auto: disk in base GB" "$current" '"disk_used":20'
check_has "units c: temperature in celsius" "$current" '"temp":50'
check_has "units bytes: net in base bytes/s" "$current" '"net_rx":5.243e+06'
check_has "units auto: memory magnitude is MB" "$metrics" '"mem":{"sym":"MB","mul":1,"div":1}'
check_has "units auto: disk magnitude is GB" "$metrics" '"disk":{"sym":"GB","mul":1,"div":1}'
check_has "units bytes: 5 MiB/s reads as KB/s" "$metrics" '"net":{"sym":"KB/s","mul":1,"div":1024}'

# Fahrenheit, and the case that separates the two net ladders: the same traffic
# is 5120 KB/s (four digits, stays put) but 41943 Kbps (five, steps up).
units_serve 'temp_card_unit = "f"
net_card_unit = "bits"
net_chart_unit = "bits"'
check_has "units f: temperature in fahrenheit" "$current" '"temp":122'
check_has "units bits: value stays in base bytes/s" "$current" '"net_rx":5.243e+06'
check_has "units bits: the same traffic steps up to Mbps" "$metrics" \
    '"net":{"sym":"Mbps","mul":8,"div":1e+06}'
expect_rc 1 "serve with missing config exits 1" "$BIN" serve --config "$work/none.toml"

printf '\n  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
