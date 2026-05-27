#!/bin/sh
# minimoni — zero-dependency system monitoring
# Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
# GPLv3+ (see src/main.c for the full header).
#
# Integration tests for minimoni-migrate. Treats the binary as a black box:
# builds test databases via the sqlite3 CLI, runs minimoni-migrate, verifies
# observable post-state.
#
# Requirements: sqlite3 CLI in PATH, ./minimoni and ./minimoni-migrate built.
# Run with: make test-migrate

set -eu

MIN=./minimoni
MIG=./minimoni-migrate

if ! [ -x "$MIN" ] || ! [ -x "$MIG" ]; then
    echo "  $MIN and $MIG must be built first (run: make)" >&2
    exit 2
fi
if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "  sqlite3 CLI not found in PATH; install it to run migrate tests" >&2
    exit 2
fi

TMP=$(mktemp -d 2>/dev/null || mktemp -d -t minimoni-migrate-test)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# Build a canonical v0.1 database (the exact schema the v0.1 daemon created
# via CREATE TABLE IF NOT EXISTS). Two rows 60 s apart, with byte counters
# that grow by known amounts so the bps reconstruction is checkable. The
# timestamp format is the legacy non-T+Z one the v0.1 daemon stored.
make_v01_db() {
    sqlite3 "$1" <<'SQL'
CREATE TABLE IF NOT EXISTS metrics (  timestamp        TEXT NOT NULL,  load_1m          REAL, load_5m REAL, load_15m REAL,  cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,  mem_total_mb     REAL, mem_used_mb REAL,  mem_available_mb REAL, mem_percent REAL,  disk_total_gb    REAL, disk_used_gb REAL,  disk_free_gb     REAL, disk_percent REAL,  temp_celsius     REAL,  net_rx_bytes     INTEGER, net_tx_bytes INTEGER,  uptime_seconds   REAL);
CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(timestamp);
CREATE TABLE IF NOT EXISTS alert_log (  alert_name TEXT NOT NULL,  fired_at   TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_alert_log_name ON alert_log(alert_name);
INSERT INTO metrics VALUES ('2026-05-26 10:00:00', 0.5,0.4,0.3, 10,5,85, 1000,400,600,40, 10,5,5,50, 45, 100000, 50000, 3600);
INSERT INTO metrics VALUES ('2026-05-26 10:01:00', 0.6,0.5,0.4, 12,6,82, 1000,410,590,41, 10,5,5,50, 46, 106000, 53000, 3660);
SQL
}

report() {
    if [ "$2" = ok ]; then
        printf '  %-50s ok\n' "$1"
        pass=$((pass + 1))
    else
        printf '  %-50s FAIL: %s\n' "$1" "$2"
        fail=$((fail + 1))
    fi
}

# --- Test cases ----------------------------------------------------------

t_basic_v01_to_v1() {
    db=$TMP/basic.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 || {
        report "basic v0->v1 (canonical schema)" "exit code != 0"
        return
    }
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    aid=$(sqlite3 "$db" 'PRAGMA application_id')
    [ "$uv" = 1 ] || { report "basic v0->v1 (canonical schema)" "user_version=$uv (want 1)"; return; }
    [ "$aid" = 1836019305 ] || {
        report "basic v0->v1 (canonical schema)" "application_id=$aid (want 1836019305)"
        return
    }
    report "basic v0->v1 (canonical schema)" ok
}

t_schema_after_migration() {
    db=$TMP/schema.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    cols=$(sqlite3 "$db" "SELECT GROUP_CONCAT(name, ',') FROM pragma_table_info('metrics')")
    # bucket_sec, net_rx_bps, net_tx_bps must be present;
    # net_rx_bytes, net_tx_bytes must be absent.
    case "$cols" in
        *bucket_sec*net_rx_bps*net_tx_bps*) ;;
        *) report "post-migration columns present" "got: $cols"; return ;;
    esac
    case "$cols" in
        *net_rx_bytes*|*net_tx_bytes*)
            report "post-migration columns present" "old byte columns still there: $cols"
            return ;;
    esac
    report "post-migration columns present" ok
}

t_timestamps_normalized() {
    db=$TMP/ts.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    ts=$(sqlite3 "$db" "SELECT timestamp FROM metrics LIMIT 1")
    case "$ts" in
        *T*Z) report "timestamps normalized to T+Z" ok ;;
        *)    report "timestamps normalized to T+Z" "got: $ts" ;;
    esac
}

t_bps_computed_from_byte_deltas() {
    db=$TMP/bps.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    # First row: no predecessor → bps NULL.
    # Second row: (106000-100000)/60 = 100.0 rx; (53000-50000)/60 = 50.0 tx.
    bps=$(sqlite3 "$db" "SELECT COALESCE(net_rx_bps,'NULL')||'|'||COALESCE(net_tx_bps,'NULL') FROM metrics ORDER BY timestamp")
    expected="NULL|NULL
100.0|50.0"
    [ "$bps" = "$expected" ] && report "bps computed from byte deltas" ok ||
        report "bps computed from byte deltas" "got: $bps"
}

t_backup_created_by_default() {
    db=$TMP/bk.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    [ -f "$db.backup-pre-migrate-v0" ] && report "backup created by default" ok ||
        report "backup created by default" "missing"
}

t_no_backup_flag() {
    db=$TMP/nobk.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" --no-backup "$db" >/dev/null 2>&1
    [ -f "$db.backup-pre-migrate-v0" ] &&
        report "--no-backup skips snapshot" "backup created anyway" ||
        report "--no-backup skips snapshot" ok
}

t_already_at_latest() {
    db=$TMP/done.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    rc=$?
    [ "$rc" = 0 ] && report "re-run on migrated DB is no-op" ok ||
        report "re-run on migrated DB is no-op" "exit=$rc (want 0)"
}

t_fingerprint_refuses_extra_column() {
    # Canonical v0.1 + one extra column → fingerprint should reject and
    # leave NO backup behind (refused BEFORE snapshot).
    db=$TMP/extra.db
    sqlite3 "$db" <<'SQL'
CREATE TABLE IF NOT EXISTS metrics (  timestamp        TEXT NOT NULL,  load_1m          REAL, load_5m REAL, load_15m REAL,  cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,  mem_total_mb     REAL, mem_used_mb REAL,  mem_available_mb REAL, mem_percent REAL,  disk_total_gb    REAL, disk_used_gb REAL,  disk_free_gb     REAL, disk_percent REAL,  temp_celsius     REAL,  net_rx_bytes     INTEGER, net_tx_bytes INTEGER,  uptime_seconds   REAL,  my_extra_col TEXT);
CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(timestamp);
CREATE TABLE IF NOT EXISTS alert_log (  alert_name TEXT NOT NULL,  fired_at   TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_alert_log_name ON alert_log(alert_name);
SQL
    # Capture migrate's exit code via the && rc=0 || rc=$? pattern; reading
    # $? after `fi` returns 0 (the if structure's status), not the command's.
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    if [ "$rc" -ne 1 ]; then
        report "fingerprint refuses extra column" "exit=$rc (want 1)"
        return
    fi
    [ -f "$db.backup-pre-migrate-v0" ] &&
        report "fingerprint refuses extra column" "backup created (should be none)" ||
        report "fingerprint refuses extra column" ok
}

t_fingerprint_refuses_missing_table() {
    # No metrics table at all → caught by preflight before fingerprint runs.
    # Documents that the preflight catches the easy cases first.
    db=$TMP/nometrics.db
    sqlite3 "$db" "CREATE TABLE foo (x INT);"
    if "$MIG" --use "$MIN" "$db" >/dev/null 2>&1; then
        report "refuses DB without metrics table" "exit=0 (want 1)"
    else
        rc=$?
        [ "$rc" = 1 ] && report "refuses DB without metrics table" ok ||
            report "refuses DB without metrics table" "exit=$rc (want 1)"
    fi
}

t_wrong_application_id() {
    db=$TMP/wrong.db
    sqlite3 "$db" "CREATE TABLE metrics (timestamp TEXT); PRAGMA application_id = 12345;"
    if "$MIG" --use "$MIN" "$db" >/dev/null 2>&1; then
        report "refuses wrong application_id" "exit=0 (want 1)"
    else
        rc=$?
        [ "$rc" = 1 ] && report "refuses wrong application_id" ok ||
            report "refuses wrong application_id" "exit=$rc (want 1)"
    fi
}

t_nonexistent_db() {
    if "$MIG" --use "$MIN" "$TMP/does-not-exist.db" >/dev/null 2>&1; then
        report "refuses non-existent DB" "exit=0 (want 1)"
    else
        rc=$?
        [ "$rc" = 1 ] && report "refuses non-existent DB" ok ||
            report "refuses non-existent DB" "exit=$rc (want 1)"
    fi
}

t_help_and_version() {
    "$MIG" --help >/dev/null 2>&1 || { report "--help and --version work" "help nonzero"; return; }
    out=$("$MIG" --version 2>/dev/null)
    case "$out" in
        "minimoni-migrate "*) report "--help and --version work" ok ;;
        *) report "--help and --version work" "version output: $out" ;;
    esac
}

t_auto_resolve_minimoni() {
    db=$TMP/auto.db
    make_v01_db "$db"
    "$MIG" "$db" >/dev/null 2>&1 || {
        report "default --use resolves colocated minimoni" "exit nonzero"
        return
    }
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    [ "$uv" = 1 ] && report "default --use resolves colocated minimoni" ok ||
        report "default --use resolves colocated minimoni" "user_version=$uv"
}

# --- Runner --------------------------------------------------------------

t_basic_v01_to_v1
t_schema_after_migration
t_timestamps_normalized
t_bps_computed_from_byte_deltas
t_backup_created_by_default
t_no_backup_flag
t_already_at_latest
t_fingerprint_refuses_extra_column
t_fingerprint_refuses_missing_table
t_wrong_application_id
t_nonexistent_db
t_help_and_version
t_auto_resolve_minimoni

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
