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

# Integration tests for minimoni-migrate. Treats the binary as a black box:
# builds test databases via the sqlite3 CLI, runs minimoni-migrate, verifies
# observable post-state.
#
# Requirements: sqlite3 CLI in PATH, ./minimoni and ./minimoni-migrate built.
# Run with: make test-migrate
#
# Scope: this is the END-TO-END suite for minimoni-migrate, not a smoke harness
# like cli.sh. Migrate's logic (preflight, version routing, structural
# fingerprint, snapshot, dry-run, --force) only runs through a live
# `minimoni db exec` fork+exec, so it cannot be unit-tested the way db_cmd can:
# behavioural depth lives here by necessity, and cli.sh's ~30-check smoke
# ceiling does NOT apply (this file's size tracks the number of real migrate
# behaviours, not scope creep). What is unit-testable in pure C (the migration
# registry, the snapshot copy) lives in tests/unit-migrate.c. Before adding a
# case, ask whether it can be a unit test there first; if it needs the live
# binary, it belongs here.

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

# Build a canonical v0.1 database: the v0.1 schema (same columns and types the
# v0.1 daemon created via CREATE TABLE IF NOT EXISTS), plus the moni
# application_id it stamps; user_version stays 0. Two rows 60 s apart, with byte
# counters that grow by known amounts so the bps reconstruction is checkable.
# Timestamps use the canonical T+Z form the daemon has written since v0.1.
make_v01_db() {
    sqlite3 "$1" <<'SQL'
CREATE TABLE IF NOT EXISTS metrics (
    timestamp TEXT NOT NULL,
    load_1m REAL, load_5m REAL, load_15m REAL,
    cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,
    mem_total_mb REAL, mem_used_mb REAL, mem_available_mb REAL, mem_percent REAL,
    disk_total_gb REAL, disk_used_gb REAL, disk_free_gb REAL, disk_percent REAL,
    temp_celsius REAL,
    net_rx_bytes INTEGER, net_tx_bytes INTEGER,
    uptime_seconds REAL);
CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(timestamp);
CREATE TABLE IF NOT EXISTS alert_log (alert_name TEXT NOT NULL, fired_at TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_alert_log_name ON alert_log(alert_name);
-- Aged rows relative to now (never a fixed date), so they always read as old.
INSERT INTO metrics VALUES
    (strftime('%Y-%m-%dT%H:%M:%SZ','now','-10 days'),
     0.5,0.4,0.3, 10,5,85, 1000,400,600,40,
     10,5,5,50, 45, 100000, 50000, 3600);
INSERT INTO metrics VALUES
    (strftime('%Y-%m-%dT%H:%M:%SZ','now','-10 days','+60 seconds'),
     0.6,0.5,0.4, 12,6,82, 1000,410,590,41,
     10,5,5,50, 46, 106000, 53000, 3660);
PRAGMA application_id = 1836019305;
SQL
}

# Build an ALTER-derived schema: a real-world v0.1 DB whose cpu_*, net_* and
# uptime_seconds columns were appended with ALTER TABLE ADD COLUMN, so they sit
# at the END of the column list, not mid-list as the canonical fresh-install
# schema has them (different whitespace too). Stamps the moni application_id,
# like every real minimoni database, so these databases clear preflight and
# exercise the structural fingerprint, which accepts them: same columns and
# types, only the physical order differs.
make_alter_derived_db() {
    sqlite3 "$1" <<'SQL'
CREATE TABLE metrics (
    timestamp TEXT NOT NULL,
    load_1m REAL, load_5m REAL, load_15m REAL,
    mem_total_mb REAL, mem_used_mb REAL, mem_available_mb REAL, mem_percent REAL,
    disk_total_gb REAL, disk_used_gb REAL, disk_free_gb REAL, disk_percent REAL,
    temp_celsius REAL
);
ALTER TABLE metrics ADD COLUMN cpu_user_percent REAL;
ALTER TABLE metrics ADD COLUMN cpu_system_percent REAL;
ALTER TABLE metrics ADD COLUMN cpu_idle_percent REAL;
ALTER TABLE metrics ADD COLUMN net_rx_bytes INTEGER;
ALTER TABLE metrics ADD COLUMN net_tx_bytes INTEGER;
ALTER TABLE metrics ADD COLUMN uptime_seconds REAL;
CREATE INDEX idx_metrics_ts ON metrics(timestamp);
CREATE TABLE alert_log (alert_name TEXT NOT NULL, fired_at TEXT NOT NULL);
CREATE INDEX idx_alert_log_name ON alert_log(alert_name);
PRAGMA application_id = 1836019305;
SQL
}

# Build a canonical v0.1 schema plus one extra column the daemon never created.
# A genuine structural divergence: the structural fingerprint must reject it,
# and --force must be able to override it. Stamps the moni application_id.
make_extra_col_db() {
    sqlite3 "$1" <<'SQL'
CREATE TABLE IF NOT EXISTS metrics (
    timestamp TEXT NOT NULL,
    load_1m REAL, load_5m REAL, load_15m REAL,
    cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,
    mem_total_mb REAL, mem_used_mb REAL, mem_available_mb REAL, mem_percent REAL,
    disk_total_gb REAL, disk_used_gb REAL, disk_free_gb REAL, disk_percent REAL,
    temp_celsius REAL,
    net_rx_bytes INTEGER, net_tx_bytes INTEGER,
    uptime_seconds REAL,
    my_extra_col TEXT);
CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(timestamp);
CREATE TABLE IF NOT EXISTS alert_log (alert_name TEXT NOT NULL, fired_at TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_alert_log_name ON alert_log(alert_name);
PRAGMA application_id = 1836019305;
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

# expect_rc NAME GOT WANT: report NAME ok if exit code GOT equals WANT.
expect_rc() {
    if [ "$2" = "$3" ]; then
        report "$1" ok
    else
        report "$1" "exit=$2 (want $3)"
    fi
}

# --- Successful migration ------------------------------------------------

t_basic_v01_to_v1() {
    db=$TMP/basic.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 || {
        report "basic v0->v1 (canonical schema)" "exit code != 0"
        return
    }
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    aid=$(sqlite3 "$db" 'PRAGMA application_id')
    if [ "$uv" != 1 ]; then
        report "basic v0->v1 (canonical schema)" "user_version=$uv (want 1)"
        return
    fi
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

t_bps_computed_from_byte_deltas() {
    db=$TMP/bps.db
    make_v01_db "$db"
    # Make the two rows recent (single shell epoch, so the 60 s gap is exact and
    # there are no fixed dates): the closing consolidation then leaves them raw,
    # isolating the bps delta calc from the fold (compaction is covered separately).
    now=$(date +%s)
    sqlite3 "$db" \
        "UPDATE metrics SET timestamp=strftime('%Y-%m-%dT%H:%M:%SZ',$((now - 120)),'unixepoch')
           WHERE net_rx_bytes=100000;
         UPDATE metrics SET timestamp=strftime('%Y-%m-%dT%H:%M:%SZ',$((now - 60)),'unixepoch')
           WHERE net_rx_bytes=106000;"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    # Row 1 has no predecessor -> bps NULL; row 2 -> (106000-100000)/60 = 100.0 rx,
    # (53000-50000)/60 = 50.0 tx.
    bps=$(sqlite3 "$db" "SELECT COALESCE(net_rx_bps,'NULL')||'|'||COALESCE(net_tx_bps,'NULL')
        FROM metrics ORDER BY timestamp")
    expected="NULL|NULL
100.0|50.0"
    if [ "$bps" = "$expected" ]; then
        report "bps computed from byte deltas" ok
    else
        report "bps computed from byte deltas" "got: $bps"
    fi
}

t_migrates_alter_derived() {
    # ALTER-derived schema (columns appended at the end): the structural
    # fingerprint ignores column order, so migrate accepts it and upgrades it
    # in place. No manual surgery needed.
    db=$TMP/alter-derived.db
    make_alter_derived_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 || {
        report "migrates ALTER-derived schema" "migrate failed"
        return
    }
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    cols=$(sqlite3 "$db" "SELECT GROUP_CONCAT(name, ',') FROM pragma_table_info('metrics')")
    if [ "$uv" != 1 ]; then
        report "migrates ALTER-derived schema" "user_version=$uv (want 1)"
        return
    fi
    case "$cols" in
        *bucket_sec*net_rx_bps*net_tx_bps*) report "migrates ALTER-derived schema" ok ;;
        *) report "migrates ALTER-derived schema" "cols: $cols" ;;
    esac
}

t_backup_created_by_default() {
    db=$TMP/bk.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    if [ -f "$db.backup-pre-migrate-v0" ]; then
        report "backup created by default" ok
    else
        report "backup created by default" "missing"
    fi
}

t_no_backup_flag() {
    db=$TMP/nobk.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" --no-backup "$db" >/dev/null 2>&1
    if [ -f "$db.backup-pre-migrate-v0" ]; then
        report "--no-backup skips snapshot" "backup created anyway"
    else
        report "--no-backup skips snapshot" ok
    fi
}

t_already_at_latest() {
    db=$TMP/done.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1
    rc=$?
    expect_rc "re-run on migrated DB is no-op" "$rc" 0
}

# --- Refusals (ordered by the check that trips) --------------------------

t_nonexistent_db() {
    if "$MIG" --use "$MIN" "$TMP/does-not-exist.db" >/dev/null 2>&1; then
        report "refuses non-existent DB" "exit=0 (want 1)"
    else
        rc=$?
        expect_rc "refuses non-existent DB" "$rc" 1
    fi
}

t_refuses_corrupted_db() {
    # Write garbage bytes that look nothing like a SQLite file. preflight's
    # integrity_check must trip and refuse before any write happens.
    db=$TMP/corrupt.db
    printf 'this is not a sqlite database, just garbage\n' > "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    expect_rc "refuses corrupted DB" "$rc" 1
}

t_wrong_application_id() {
    db=$TMP/wrong.db
    sqlite3 "$db" "CREATE TABLE metrics (timestamp TEXT); PRAGMA application_id = 12345;"
    if "$MIG" --use "$MIN" "$db" >/dev/null 2>&1; then
        report "refuses wrong application_id" "exit=0 (want 1)"
    else
        rc=$?
        expect_rc "refuses wrong application_id" "$rc" 1
    fi
}

t_fingerprint_refuses_missing_table() {
    # No metrics table at all -> caught by preflight before fingerprint runs.
    # Documents that the preflight catches the easy cases first.
    db=$TMP/nometrics.db
    sqlite3 "$db" "CREATE TABLE foo (x INT); PRAGMA application_id = 1836019305;"
    if "$MIG" --use "$MIN" "$db" >/dev/null 2>&1; then
        report "refuses DB without metrics table" "exit=0 (want 1)"
    else
        rc=$?
        expect_rc "refuses DB without metrics table" "$rc" 1
    fi
}

t_refuses_unknown_user_version() {
    # DB at a user_version this build does not know about (e.g. someone
    # downgraded the binary). Must refuse with exit=1, never silently roll
    # back the schema.
    db=$TMP/future.db
    sqlite3 "$db" <<'SQL'
CREATE TABLE metrics (timestamp TEXT NOT NULL);
PRAGMA application_id = 1836019305;
PRAGMA user_version = 99;
SQL
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    expect_rc "refuses user_version > latest" "$rc" 1
}

t_fingerprint_refuses_extra_column() {
    # Canonical v0.1 + one extra column is a genuine structural divergence ->
    # the structural fingerprint must reject and leave NO backup behind
    # (refused BEFORE snapshot).
    db=$TMP/extra.db
    make_extra_col_db "$db"
    # Capture migrate's exit code via the && rc=0 || rc=$? pattern; reading
    # $? after `fi` returns 0 (the if structure's status), not the command's.
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    if [ "$rc" -ne 1 ]; then
        report "fingerprint refuses extra column" "exit=$rc (want 1)"
        return
    fi
    if [ -f "$db.backup-pre-migrate-v0" ]; then
        report "fingerprint refuses extra column" "backup created (should be none)"
    else
        report "fingerprint refuses extra column" ok
    fi
}

t_fingerprint_refuses_wrong_type() {
    # Canonical v0.1 columns but load_1m declared TEXT instead of REAL. The
    # structural fingerprint includes each column's type, so this is a genuine
    # divergence: it must be refused with no backup. Proves the type (not just
    # the column name) is part of the fingerprint.
    db=$TMP/wrongtype.db
    sqlite3 "$db" <<'SQL'
CREATE TABLE metrics (
    timestamp TEXT NOT NULL,
    load_1m TEXT, load_5m REAL, load_15m REAL,
    cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,
    mem_total_mb REAL, mem_used_mb REAL, mem_available_mb REAL, mem_percent REAL,
    disk_total_gb REAL, disk_used_gb REAL, disk_free_gb REAL, disk_percent REAL,
    temp_celsius REAL,
    net_rx_bytes INTEGER, net_tx_bytes INTEGER,
    uptime_seconds REAL);
CREATE INDEX idx_metrics_ts ON metrics(timestamp);
CREATE TABLE alert_log (alert_name TEXT NOT NULL, fired_at TEXT NOT NULL);
CREATE INDEX idx_alert_log_name ON alert_log(alert_name);
PRAGMA application_id = 1836019305;
SQL
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    if [ "$rc" -ne 1 ]; then
        report "fingerprint refuses wrong column type" "exit=$rc (want 1)"
        return
    fi
    if [ -f "$db.backup-pre-migrate-v0" ]; then
        report "fingerprint refuses wrong column type" "backup created (should be none)"
    else
        report "fingerprint refuses wrong column type" ok
    fi
}

# --- Dry-run (--dry-run rehearses on a throwaway copy) -------------------

t_dry_run_pending_migration() {
    # Canonical v0.1 DB with a pending migration. Dry-run must report
    # migration-pending on stdout, exit 0, and leave the DB completely
    # untouched: user_version still 0, no backup, no leftover temp file.
    db=$TMP/dry-pending.db
    make_v01_db "$db"
    out=$("$MIG" --use "$MIN" --dry-run "$db" 2>/dev/null) && rc=0 || rc=$?
    if [ "$rc" -ne 0 ]; then
        report "dry-run reports migration-pending" "exit=$rc (want 0)"
        return
    fi
    if [ "$out" != "status: migration-pending" ]; then
        report "dry-run reports migration-pending" "stdout='$out'"
        return
    fi
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    if [ "$uv" != 0 ]; then
        report "dry-run reports migration-pending" "DB mutated (uv=$uv)"
        return
    fi
    if [ -f "$db.backup-pre-migrate-v0" ]; then
        report "dry-run reports migration-pending" "left a backup behind"
        return
    fi
    # No stray dry-run copy left in the directory.
    leftovers=$(find "$TMP" -name 'dry-pending.db.dry-run-*' 2>/dev/null)
    if [ -z "$leftovers" ]; then
        report "dry-run reports migration-pending" ok
    else
        report "dry-run reports migration-pending" "left temp copy: $leftovers"
    fi
}

t_dry_run_pending_alter_derived() {
    # ALTER-derived migrates under the structural fingerprint, so a dry-run
    # reports migration-pending (not blocked) and leaves the DB untouched.
    db=$TMP/dry-alter.db
    make_alter_derived_db "$db"
    out=$("$MIG" --use "$MIN" --dry-run "$db" 2>/dev/null) && rc=0 || rc=$?
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    if [ "$rc" = 0 ] && [ "$out" = "status: migration-pending" ] && [ "$uv" = 0 ]; then
        report "dry-run reports migration-pending on ALTER-derived" ok
    else
        report "dry-run reports migration-pending on ALTER-derived" "exit=$rc stdout='$out' uv=$uv"
    fi
}

t_dry_run_up_to_date() {
    # Migrate a DB for real, then dry-run it again: should report up-to-date,
    # exit 0.
    db=$TMP/dry-current.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 || {
        report "dry-run reports up-to-date" "setup migration failed"
        return
    }
    out=$("$MIG" --use "$MIN" --dry-run "$db" 2>/dev/null) && rc=0 || rc=$?
    if [ "$rc" = 0 ] && [ "$out" = "status: up-to-date" ]; then
        report "dry-run reports up-to-date" ok
    else
        report "dry-run reports up-to-date" "exit=$rc stdout='$out'"
    fi
}

t_dry_run_blocked_corrupt() {
    db=$TMP/dry-corrupt.db
    printf 'this is not a sqlite database, just garbage\n' > "$db"
    out=$("$MIG" --use "$MIN" --dry-run "$db" 2>/dev/null) && rc=0 || rc=$?
    if [ "$rc" = 1 ] && [ "$out" = "status: blocked" ]; then
        report "dry-run reports blocked on corrupt DB" ok
    else
        report "dry-run reports blocked on corrupt DB" "exit=$rc stdout='$out'"
    fi
}

t_dry_run_blocked_structural() {
    # dry-run on a genuine structural divergence (extra column): unlike the
    # corrupt case, preflight passes and the fingerprint is what blocks. Must
    # report blocked, exit 1, and leave the DB untouched.
    db=$TMP/dry-structural.db
    make_extra_col_db "$db"
    out=$("$MIG" --use "$MIN" --dry-run "$db" 2>/dev/null) && rc=0 || rc=$?
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    if [ "$rc" = 1 ] && [ "$out" = "status: blocked" ] && [ "$uv" = 0 ]; then
        report "dry-run reports blocked on structural divergence" ok
    else
        report "dry-run reports blocked on structural divergence" "exit=$rc stdout='$out' uv=$uv"
    fi
}

# --- --force (skip the structural fingerprint for a reviewed schema) -----

t_force_migrates_past_fingerprint() {
    # A genuine structural divergence (extra column) is refused by default,
    # but --force migrates it anyway. user_version becomes 1.
    db=$TMP/force.db
    make_extra_col_db "$db"
    "$MIG" --use "$MIN" "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    if [ "$rc" -ne 1 ]; then
        report "--force migrates past fingerprint" "expected refuse without --force (got $rc)"
        return
    fi
    "$MIG" --use "$MIN" --force "$db" >/dev/null 2>&1 || {
        report "--force migrates past fingerprint" "--force run failed"
        return
    }
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    if [ "$uv" = 1 ]; then
        report "--force migrates past fingerprint" ok
    else
        report "--force migrates past fingerprint" "user_version=$uv (want 1)"
    fi
}

t_force_rejects_no_backup() {
    # --force mandates a backup; combining it with --no-backup is an error.
    db=$TMP/force-nobk.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" --force --no-backup "$db" >/dev/null 2>&1 && rc=0 || rc=$?
    expect_rc "--force rejects --no-backup" "$rc" 1
}

# --- CLI -----------------------------------------------------------------

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
    if [ "$uv" = 1 ]; then
        report "default --use resolves colocated minimoni" ok
    else
        report "default --use resolves colocated minimoni" "user_version=$uv"
    fi
}

# --- migration compaction (v0.1 -> v0.2 folds the raw backlog and VACUUMs) ---

t_migration_consolidates_and_compacts() {
    db=$TMP/compact.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" --no-backup "$db" >/dev/null 2>&1 || {
        report "migration folds the backlog and compacts" "exit code != 0"
        return
    }
    uv=$(sqlite3 "$db" 'PRAGMA user_version')
    if [ "$uv" != 1 ]; then
        report "migration folds the backlog and compacts" "user_version=$uv (want 1)"
        return
    fi
    # Every aged row was folded into a tier: none left raw (bucket_sec NULL).
    raw=$(sqlite3 "$db" 'SELECT COUNT(*) FROM metrics WHERE bucket_sec IS NULL' 2>/dev/null)
    if [ "$raw" != 0 ]; then
        report "migration folds the backlog and compacts" "$raw rows left un-consolidated"
        return
    fi
    # The closing VACUUM returns the freed pages to the filesystem: free list empty.
    fl=$(sqlite3 "$db" 'PRAGMA freelist_count' 2>/dev/null)
    if [ "$fl" != 0 ]; then
        report "migration folds the backlog and compacts" "freelist_count=$fl (want 0)"
        return
    fi
    report "migration folds the backlog and compacts" ok
}

t_remigrate_is_noop() {
    db=$TMP/compact-noop.db
    make_v01_db "$db"
    "$MIG" --use "$MIN" --no-backup "$db" >/dev/null 2>&1
    # Re-running on an already-migrated DB must be a no-op: exit 0, no work.
    out=$("$MIG" --use "$MIN" --no-backup "$db" 2>&1) && rc=0 || rc=$?
    if [ "$rc" != 0 ]; then
        report "re-migrate on an up-to-date DB is a no-op" "exit=$rc (want 0)"
        return
    fi
    case "$out" in
        *"already at the latest"*) report "re-migrate on an up-to-date DB is a no-op" ok ;;
        *) report "re-migrate on an up-to-date DB is a no-op" "no 'already at latest' message" ;;
    esac
}

# --- Runner --------------------------------------------------------------

t_basic_v01_to_v1
t_schema_after_migration
t_bps_computed_from_byte_deltas
t_migrates_alter_derived
t_backup_created_by_default
t_no_backup_flag
t_already_at_latest
t_nonexistent_db
t_refuses_corrupted_db
t_wrong_application_id
t_fingerprint_refuses_missing_table
t_refuses_unknown_user_version
t_fingerprint_refuses_extra_column
t_fingerprint_refuses_wrong_type
t_dry_run_pending_migration
t_dry_run_pending_alter_derived
t_dry_run_up_to_date
t_dry_run_blocked_corrupt
t_dry_run_blocked_structural
t_force_migrates_past_fingerprint
t_force_rejects_no_backup
t_help_and_version
t_auto_resolve_minimoni
t_migration_consolidates_and_compacts
t_remigrate_is_noop

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
