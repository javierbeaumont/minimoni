/*
 * minimoni - zero-dependency system monitoring
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* Unit tests for src/db.c: db_open's validate-on-open state machine and the
 * write-time tiered consolidation (db_consolidate). Zero-dependency, no
 * framework; build with `make test`. The module under test is #included
 * directly so its helpers are exercisable, and the suite links
 * vendor/sqlite3.c to drive both against a real database.
 *
 * Rows are inserted with explicit timestamps (and, for the per-transition
 * regression tests, explicit bucket_sec) so consolidation can be exercised at
 * a chosen age without waiting. Assertions count rows by bucket_sec, which is
 * independent of the timestamp text. Timestamps are written in the canonical
 * T+Z format the daemon stores, so the query-path test exercises the real
 * comparison against the iso_cutoff bound. */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "runner.h"

#include "../src/db.c"

/* --- Consolidation infrastructure --- */

static int  g_db_counter = 0;
static char g_tmpdb_path[256];

static int open_test_db(db_t *db)
{
    snprintf(g_tmpdb_path, sizeof(g_tmpdb_path), "/tmp/minimoni-test-db-%d-%d.db", getpid(),
             g_db_counter++);
    char wal[280], shm[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    snprintf(shm, sizeof(shm), "%s-shm", g_tmpdb_path);
    unlink(g_tmpdb_path);
    unlink(wal);
    unlink(shm);
    return db_open(db, g_tmpdb_path, 60);
}

static void close_test_db(db_t *db)
{
    db_close(db);
    char wal[280], shm[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    snprintf(shm, sizeof(shm), "%s-shm", g_tmpdb_path);
    unlink(g_tmpdb_path);
    unlink(wal);
    unlink(shm);
}

/* Insert a raw metrics row (bucket_sec left NULL) at an explicit unix
 * timestamp, in the canonical T+Z format. All numeric fields get plausible
 * non-zero values so AVG() has data to work with (net_rx_bps = 100). */
static int insert_raw_row(sqlite3 *h, long unix_ts)
{
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO metrics ("
             "  timestamp, load_1m, load_5m, load_15m,"
             "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
             "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
             "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
             "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds"
             ") VALUES ("
             "  strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ',%ld,'unixepoch'),"
             "  1.0,1.0,1.0,"
             "  50.0,5.0,45.0,"
             "  1000.0,500.0,500.0,50.0,"
             "  10.0,5.0,5.0,50.0,"
             "  42.0,100.0,200.0,1000.0"
             ");",
             unix_ts);
    return sqlite3_exec(h, sql, NULL, NULL, NULL);
}

/* Insert a row with an explicit bucket_sec value, used by the per-transition
 * regression tests to simulate "rows already in tier N" without going through
 * earlier consolidation passes. */
static int insert_tier_row(sqlite3 *h, long unix_ts, int bucket_sec_value)
{
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO metrics ("
             "  timestamp, load_1m, load_5m, load_15m,"
             "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
             "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
             "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
             "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds, bucket_sec"
             ") VALUES ("
             "  strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ',%ld,'unixepoch'),"
             "  1.0,1.0,1.0,"
             "  50.0,5.0,45.0,"
             "  1000.0,500.0,500.0,50.0,"
             "  10.0,5.0,5.0,50.0,"
             "  42.0,100.0,200.0,1000.0,%d"
             ");",
             unix_ts, bucket_sec_value);
    return sqlite3_exec(h, sql, NULL, NULL, NULL);
}

static int count_rows(sqlite3 *h, const char *where)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM metrics WHERE %s", where);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int approx(double a, double b)
{
    double d = a - b;
    return (d < 0 ? -d : d) < 1e-6;
}

/* Build a metrics_t with known, non-zero values; the *_valid flags select
 * whether cpu / temp / net are populated or left invalid (stored as NULL). */
static metrics_t sample_metrics(int cpu_valid, int temp_valid, int net_valid)
{
    metrics_t m;
    memset(&m, 0, sizeof(m));
    m.load_1m = 1.5;
    m.load_5m = 1.0;
    m.load_15m = 0.5;
    m.cpu_valid = cpu_valid;
    m.cpu_user_percent = 30.0;
    m.cpu_system_percent = 10.0;
    m.cpu_idle_percent = 60.0;
    m.mem_total_mb = 1000.0;
    m.mem_used_mb = 400.0;
    m.mem_available_mb = 600.0;
    m.mem_percent = 40.0;
    m.disk_total_gb = 20.0;
    m.disk_used_gb = 5.0;
    m.disk_free_gb = 15.0;
    m.disk_percent = 25.0;
    m.temp_valid = temp_valid;
    m.temp_celsius = 42.5;
    m.net_valid = net_valid;
    m.net_rx_bps = 1000.0;
    m.net_tx_bps = 2000.0;
    m.uptime_seconds = 123456.0;
    return m;
}

/* Insert a raw row at an explicit timestamp with a chosen load_1m and either
 * valued or NULL cpu columns, for the bucketed-AVG / NULL-handling test. */
static int insert_valued_row(sqlite3 *h, long unix_ts, double load_1m, int cpu_null)
{
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO metrics ("
             "  timestamp, load_1m, load_5m, load_15m,"
             "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
             "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
             "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
             "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds, bucket_sec"
             ") VALUES ("
             "  strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ',%ld,'unixepoch'),"
             "  %f,1.0,1.0,"
             "  %s,"
             "  1000.0,500.0,500.0,50.0,"
             "  10.0,5.0,5.0,50.0,"
             "  42.0,100.0,200.0,1000.0,60"
             ");",
             unix_ts, load_1m, cpu_null ? "NULL,NULL,NULL" : "50.0,5.0,45.0");
    return sqlite3_exec(h, sql, NULL, NULL, NULL);
}

/* --- db_insert and query round-trip --- */

/* A fully-valid metrics_t survives db_insert -> db_current unchanged, and the
 * raw row carries bucket_sec = the configured collect interval (60). */
static int test_db_insert_roundtrip(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    metrics_t m = sample_metrics(1, 1, 1);
    int       ok = (db_insert(&db, &m) == 0);

    db_row_t row;
    ok = ok && (db_current(&db, &row) == 0);
    ok = ok && approx(row.load_1m, 1.5) && approx(row.load_15m, 0.5);
    ok = ok && row.cpu_valid && approx(row.cpu_user_percent, 30.0);
    ok = ok && approx(row.mem_used_mb, 400.0) && approx(row.disk_free_gb, 15.0);
    ok = ok && row.temp_valid && approx(row.temp_celsius, 42.5);
    ok = ok && row.net_valid && approx(row.net_rx_bps, 1000.0) && approx(row.net_tx_bps, 2000.0);
    ok = ok && approx(row.uptime_seconds, 123456.0);
    ok = ok && (count_rows(db.handle, "bucket_sec = 60") == 1);

    close_test_db(&db);
    return ok ? 0 : 1;
}

/* Invalid cpu / temp / net are stored as NULL and read back as not-valid;
 * the always-collected fields (load, mem, disk) are unaffected. */
static int test_db_insert_null_gating(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    metrics_t m = sample_metrics(0, 0, 0);
    int       ok = (db_insert(&db, &m) == 0);

    db_row_t row;
    ok = ok && (db_current(&db, &row) == 0);
    ok = ok && !row.cpu_valid && !row.temp_valid && !row.net_valid;
    ok = ok && approx(row.load_1m, 1.5) && approx(row.mem_used_mb, 400.0);

    close_test_db(&db);
    return ok ? 0 : 1;
}

/* A bucketed query averages each column over the bucket, and AVG() skips NULLs:
 * three rows with load_1m 1/2/3 average to 2.0, and cpu averages only the two
 * non-NULL rows (50.0), staying valid. */
static int test_db_query_range_bucketed_avg(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long bucket = (time(NULL) / 300) * 300;
    int  ok = 1;
    ok = ok && insert_valued_row(db.handle, bucket + 0, 1.0, 0) == SQLITE_OK;
    ok = ok && insert_valued_row(db.handle, bucket + 60, 2.0, 0) == SQLITE_OK;
    ok = ok && insert_valued_row(db.handle, bucket + 120, 3.0, 1) == SQLITE_OK;

    db_row_t *rows = NULL;
    int       n = db_query_range(&db, 600, 300, &rows);
    ok = ok && (n == 1) && rows != NULL;
    ok = ok && approx(rows[0].load_1m, 2.0);
    ok = ok && rows[0].cpu_valid && approx(rows[0].cpu_user_percent, 50.0);
    free(rows);

    close_test_db(&db);
    return ok ? 0 : 1;
}

/* --- Consolidation: positive cases --- */

static int test_consolidate_basic(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    /* 5 raw rows in a single bucket, 7 days ago - past the T2->T3 threshold
     * (5 d). At interval=60s the finer tiers (5s/30s) are inert, so the first
     * consolidation that fires is into 5-min buckets (bucket_sec=300). */
    long bucket = ((time(NULL) - 7 * 86400) / 300) * 300;
    for (int i = 0; i < 5; i++) {
        if (insert_raw_row(db.handle, bucket + i * 60) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int medium = count_rows(db.handle, "bucket_sec = 300");
    int raw = count_rows(db.handle, "bucket_sec IS NULL OR bucket_sec < 300");
    close_test_db(&db);

    return (medium == 1 && raw == 0) ? 0 : 1;
}

static int test_consolidate_multiple_buckets(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    /* 15 rows spanning 3 adjacent 5-min buckets, all 7 days ago. */
    long base = ((time(NULL) - 7 * 86400) / 300) * 300;
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < 5; i++) {
            if (insert_raw_row(db.handle, base + b * 300 + i * 60) != SQLITE_OK) {
                close_test_db(&db);
                return 1;
            }
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int medium = count_rows(db.handle, "bucket_sec = 300");
    int raw = count_rows(db.handle, "bucket_sec IS NULL OR bucket_sec < 300");
    close_test_db(&db);

    return (medium == 3 && raw == 0) ? 0 : 1;
}

static int test_consolidate_idempotent(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long bucket = ((time(NULL) - 7 * 86400) / 300) * 300;
    for (int i = 0; i < 5; i++) {
        if (insert_raw_row(db.handle, bucket + i * 60) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    /* Five back-to-back consolidate cycles must not produce duplicate medium
     * rows. The bucket_sec < 300 predicate excludes already-consolidated rows,
     * so subsequent passes are no-ops. */
    for (int i = 0; i < 5; i++) {
        if (db_consolidate(&db) != 0) {
            close_test_db(&db);
            return 1;
        }
    }

    int medium = count_rows(db.handle, "bucket_sec = 300");
    int raw = count_rows(db.handle, "bucket_sec IS NULL OR bucket_sec < 300");
    close_test_db(&db);

    return (medium == 1 && raw == 0) ? 0 : 1;
}

static int test_consolidate_recent_bucket_skipped(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    /* 5 rows in a bucket from one hour ago - far younger than every threshold,
     * so nothing is eligible and the rows stay raw. */
    long bucket = ((time(NULL) - 3600) / 300) * 300;
    for (int i = 0; i < 5; i++) {
        if (insert_raw_row(db.handle, bucket + i * 60) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int medium = count_rows(db.handle, "bucket_sec = 300");
    int raw = count_rows(db.handle, "bucket_sec IS NULL OR bucket_sec < 300");
    close_test_db(&db);

    return (medium == 0 && raw == 5) ? 0 : 1;
}

/* Range query returns consolidated rows. After consolidation the medium row
 * carries the canonical T+Z timestamp; a space-format timestamp would sort
 * before the iso_cutoff bound and be dropped, so this proves the consolidate
 * INSERT writes the right format. */
static int test_consolidate_rows_queryable(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long bucket = ((time(NULL) - 7 * 86400) / 300) * 300;
    for (int i = 0; i < 5; i++) {
        if (insert_raw_row(db.handle, bucket + i * 60) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }
    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    db_row_t *rows = NULL;
    int       n = db_query_range(&db, 8L * 86400, 300, &rows);
    int       ok = (n == 1 && rows != NULL && rows[0].net_valid && rows[0].net_rx_bps > 99.0 &&
                    rows[0].net_rx_bps < 101.0);
    free(rows);
    close_test_db(&db);

    return ok ? 0 : 1;
}

/* --- Bucket-end predicate regression tests ---
 *
 * The WHERE clause consolidates on the bucket BOUNDARY, not on each row's
 * timestamp. Each test below inserts 5 rows in a destination-tier bucket whose
 * end strictly straddles the threshold age (bucket_start <= now-threshold <
 * bucket_end), with explicit bucket_sec to isolate the targeted transition.
 * A correct bucket-end predicate keeps all 5 rows at the source tier; a
 * row-level predicate (timestamp < now-threshold) would promote the subset
 * before the boundary and leave fewer than 5 behind.
 *
 * Edge case (~0.3 % of wall-clock seconds): when now-threshold is exactly a
 * multiple of the destination bucket, the two predicates agree and the test
 * passes vacuously. */

static int test_consolidate_bucket_straddles_threshold(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long X = time(NULL) - 5 * 86400; /* T2->T3 threshold */
    long bucket = (X / 300) * 300;

    for (int i = 0; i < 5; i++) {
        if (insert_raw_row(db.handle, bucket + i * 60) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int medium = count_rows(db.handle, "bucket_sec = 300");
    int raw = count_rows(db.handle, "bucket_sec IS NULL OR bucket_sec < 300");
    close_test_db(&db);

    return (medium == 0 && raw == 5) ? 0 : 1;
}

static int test_consolidate_straddles_raw_t1(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long X = time(NULL) - 2 * 3600; /* threshold age */
    long bucket = (X / 5) * 5;      /* destination 5-s bucket */

    for (int i = 0; i < 5; i++) {
        if (insert_tier_row(db.handle, bucket + i, 1) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int t1 = count_rows(db.handle, "bucket_sec = 5");
    int source = count_rows(db.handle, "bucket_sec = 1");
    close_test_db(&db);

    return (t1 == 0 && source == 5) ? 0 : 1;
}

static int test_consolidate_straddles_t1_t2(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long X = time(NULL) - 12 * 3600;
    long bucket = (X / 30) * 30;

    for (int i = 0; i < 5; i++) {
        if (insert_tier_row(db.handle, bucket + i * 5, 5) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int t2 = count_rows(db.handle, "bucket_sec = 30");
    int source = count_rows(db.handle, "bucket_sec = 5");
    close_test_db(&db);

    return (t2 == 0 && source == 5) ? 0 : 1;
}

static int test_consolidate_straddles_t3_t4(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long X = time(NULL) - 60L * 86400;
    long bucket = (X / 3600) * 3600;

    for (int i = 0; i < 5; i++) {
        if (insert_tier_row(db.handle, bucket + i * 300, 300) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int t4 = count_rows(db.handle, "bucket_sec = 3600");
    int source = count_rows(db.handle, "bucket_sec = 300");
    close_test_db(&db);

    return (t4 == 0 && source == 5) ? 0 : 1;
}

static int test_consolidate_straddles_t4_t5(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long X = time(NULL) - 365L * 86400;
    long bucket = (X / 21600) * 21600;

    for (int i = 0; i < 5; i++) {
        if (insert_tier_row(db.handle, bucket + i * 3600, 3600) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    if (db_consolidate(&db) != 0) {
        close_test_db(&db);
        return 1;
    }

    int t5 = count_rows(db.handle, "bucket_sec = 21600");
    int source = count_rows(db.handle, "bucket_sec = 3600");
    close_test_db(&db);

    return (t5 == 0 && source == 5) ? 0 : 1;
}

/* --- db_open state machine (validate-on-open) -------------------------- */

/* These tests drive db_open against each incoming database state and assert on
 * the return code AND that the metadata pragmas are untouched on the refusal
 * paths. The unmigrated-v0.1, foreign and outdated cases guard a regression
 * where the daemon advanced the pragmas before failing prepare, leaving the DB
 * in an inconsistent "user_version=1 but schema=v0" state. */

static void temp_db_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "/tmp/minimoni-test-dbopen-%d-%d.db", getpid(), g_db_counter++);
}

static void cleanup_db(const char *path)
{
    char wal[1024], shm[1024];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);
    unlink(path);
    unlink(wal);
    unlink(shm);
}

/* Execute a SQL script on `path` using a fresh sqlite handle. Returns 0. */
static int sql_exec(const char *path, const char *sql)
{
    sqlite3 *h = NULL;
    if (sqlite3_open(path, &h) != SQLITE_OK) {
        sqlite3_close(h);
        return -1;
    }
    int rc = sqlite3_exec(h, sql, NULL, NULL, NULL);
    sqlite3_close(h);
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* Read PRAGMA `name` into *out as long. Returns 0 on success. */
static int read_pragma(const char *path, const char *name, long *out)
{
    sqlite3 *h = NULL;
    if (sqlite3_open(path, &h) != SQLITE_OK) {
        sqlite3_close(h);
        return -1;
    }
    char query[64];
    snprintf(query, sizeof(query), "PRAGMA %s", name);
    sqlite3_stmt *stmt = NULL;
    int           rc = -1;
    if (sqlite3_prepare_v2(h, query, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *out = (long)sqlite3_column_int64(stmt, 0);
            rc = 0;
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(h);
    return rc;
}

/* DB resembling minimoni v0.1: a metrics table without bucket_sec/net_*bps,
 * user_version still 0. Stamps the moni application_id because the v0.1 daemon
 * already does, so this is "needs migrate", not "foreign". */
static int make_db_v01(const char *path)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "PRAGMA application_id = %d;"
             "CREATE TABLE metrics (timestamp TEXT NOT NULL, load_1m REAL);"
             "INSERT INTO metrics VALUES (strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ','now'), 0.5);",
             MINIMONI_APPLICATION_ID);
    return sql_exec(path, sql);
}

/* DB at the current v0.2 schema with the correct pragmas. */
static int make_db_v02(const char *path)
{
    char sql[2048];
    snprintf(sql, sizeof(sql),
             "PRAGMA application_id = %d;"
             "PRAGMA user_version = %d;"
             "CREATE TABLE metrics ("
             "  timestamp TEXT NOT NULL,"
             "  load_1m REAL, load_5m REAL, load_15m REAL,"
             "  cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,"
             "  mem_total_mb REAL, mem_used_mb REAL, mem_available_mb REAL, mem_percent REAL,"
             "  disk_total_gb REAL, disk_used_gb REAL, disk_free_gb REAL, disk_percent REAL,"
             "  temp_celsius REAL,"
             "  net_rx_bps REAL, net_tx_bps REAL,"
             "  uptime_seconds REAL,"
             "  bucket_sec INTEGER"
             ");"
             "CREATE INDEX idx_metrics_ts ON metrics(timestamp);"
             "CREATE TABLE alert_log (alert_name TEXT NOT NULL, fired_at TEXT NOT NULL);"
             "CREATE INDEX idx_alert_log_name ON alert_log(alert_name);",
             MINIMONI_APPLICATION_ID, MINIMONI_SCHEMA_VERSION);
    return sql_exec(path, sql);
}

/* DB belonging to another application (custom application_id, not moni). */
static int make_db_foreign(const char *path, int app_id)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "PRAGMA application_id = %d;"
             "CREATE TABLE metrics (timestamp TEXT);",
             app_id);
    return sql_exec(path, sql);
}

/* minimoni-flavoured DB with a user_version unknown to this build. */
static int make_db_outdated(const char *path, int version)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "PRAGMA application_id = %d;"
             "PRAGMA user_version = %d;"
             "CREATE TABLE metrics (timestamp TEXT);",
             MINIMONI_APPLICATION_ID, version);
    return sql_exec(path, sql);
}

/* 1. Fresh install: no file, so the daemon creates the schema and writes the
 *    version pragmas. */
static int test_db_open_fresh_install(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  ok = (rc == 0);
    if (ok) {
        long app_id = -1, version = -1;
        ok = (read_pragma(path, "application_id", &app_id) == 0 &&
              app_id == MINIMONI_APPLICATION_ID &&
              read_pragma(path, "user_version", &version) == 0 &&
              version == MINIMONI_SCHEMA_VERSION);
        db_close(&db);
    }
    cleanup_db(path);
    return ok ? 0 : 1;
}

/* 2. Pre-existing v0.2 DB with correct pragmas: daemon proceeds silently. */
static int test_db_open_existing_v02_ok(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);
    if (make_db_v02(path) != 0) {
        cleanup_db(path);
        return 1;
    }

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  ok = (rc == 0);
    if (ok) {
        long app_id = -1, version = -1;
        ok = (read_pragma(path, "application_id", &app_id) == 0 &&
              app_id == MINIMONI_APPLICATION_ID &&
              read_pragma(path, "user_version", &version) == 0 &&
              version == MINIMONI_SCHEMA_VERSION);
        db_close(&db);
    }
    cleanup_db(path);
    return ok ? 0 : 1;
}

/* 3. minimoni v0.1 DB (moni stamped, user_version=0, schema lacks the v0.2
 *    columns): daemon refuses and tells the operator to run migrate, without
 *    advancing the pragmas. */
static int test_db_open_unmigrated_v01(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);
    if (make_db_v01(path) != 0) {
        cleanup_db(path);
        return 1;
    }

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  refused = (rc != 0);

    long app_id = -1, version = -1;
    int  pragmas_unchanged =
        (read_pragma(path, "application_id", &app_id) == 0 && app_id == MINIMONI_APPLICATION_ID &&
         read_pragma(path, "user_version", &version) == 0 && version == 0);

    cleanup_db(path);
    return (refused && pragmas_unchanged) ? 0 : 1;
}

/* 4. Foreign DB (application_id not moni): daemon refuses, leaves the pragmas
 *    alone. */
static int test_db_open_foreign_app_id(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);
    if (make_db_foreign(path, 12345) != 0) {
        cleanup_db(path);
        return 1;
    }

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  refused = (rc != 0);

    long app_id = -1;
    int  pragmas_unchanged = (read_pragma(path, "application_id", &app_id) == 0 && app_id == 12345);

    cleanup_db(path);
    return (refused && pragmas_unchanged) ? 0 : 1;
}

/* 5. minimoni DB with an unknown future user_version: daemon refuses. */
static int test_db_open_outdated_minimoni(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);
    if (make_db_outdated(path, 99) != 0) {
        cleanup_db(path);
        return 1;
    }

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  refused = (rc != 0);

    long version = -1;
    int  pragmas_unchanged = (read_pragma(path, "user_version", &version) == 0 && version == 99);

    cleanup_db(path);
    return (refused && pragmas_unchanged) ? 0 : 1;
}

/* 6. Repeated failed opens never advance the pragmas, even after many
 *    attempts. */
static int test_db_open_pragmas_unchanged_on_refuse(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);
    if (make_db_v01(path) != 0) {
        cleanup_db(path);
        return 1;
    }

    db_t db;
    for (int i = 0; i < 5; i++)
        (void)db_open(&db, path, 60);

    long app_id = -1, version = -1;
    int  ok =
        (read_pragma(path, "application_id", &app_id) == 0 && app_id == MINIMONI_APPLICATION_ID &&
         read_pragma(path, "user_version", &version) == 0 && version == 0);

    cleanup_db(path);
    return ok ? 0 : 1;
}

/* 7. File exists but contains garbage (not a SQLite file): no crash. */
static int test_db_open_corrupt_file(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 1;
    const char garbage[] = "This is not a SQLite database, just garbage bytes\n";
    if (write(fd, garbage, sizeof(garbage) - 1) < 0) {
        close(fd);
        cleanup_db(path);
        return 1;
    }
    close(fd);

    db_t db;
    int  rc = db_open(&db, path, 60);
    /* Either accepted (SQLite is permissive about file format on some
     * versions) or refused: both are non-crashing, which is what we test. If
     * accepted, close cleanly. */
    if (rc == 0)
        db_close(&db);

    cleanup_db(path);
    return 0;
}

/* 8. Path points at a directory: sqlite3_open fails, db_open returns non-zero,
 *    no crash. */
static int test_db_open_directory_as_path(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/minimoni-test-dbopen-dir-%d", getpid());
    rmdir(path);
    if (mkdir(path, 0755) != 0)
        return 1;

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  refused = (rc != 0);

    rmdir(path);
    return refused ? 0 : 1;
}

/* 9. Fresh install + close + reopen: the second open goes through the
 *    existed_before branch and proceeds OK. */
static int test_db_open_reopen_after_fresh(void)
{
    char path[256];
    temp_db_path(path, sizeof(path));
    cleanup_db(path);

    db_t db;
    int  rc1 = db_open(&db, path, 60);
    if (rc1 != 0) {
        cleanup_db(path);
        return 1;
    }
    db_close(&db);

    int rc2 = db_open(&db, path, 60);
    int ok = (rc2 == 0);
    if (ok)
        db_close(&db);

    cleanup_db(path);
    return ok ? 0 : 1;
}

/* 10. Path points inside a non-existent directory: sqlite3_open fails, no .db
 *     file ever gets created. */
static int test_db_open_path_in_nonexistent_dir(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/minimoni-nonexistent-%d/foo.db", getpid());

    db_t db;
    int  rc = db_open(&db, path, 60);
    int  refused = (rc != 0);

    return refused ? 0 : 1;
}

/* --- Runner --- */

/* --- One cutoff instant per pass ---
 *
 * Regression tests for the INSERT and the DELETE of a tier landing on
 * different seconds (see build_consolidate_sql). Passing the instant in also
 * takes the wall clock out of the tests: they pin it instead of seeding
 * relative to time(NULL) and hoping the boundary falls where they need it.
 *
 * PINNED_NOW is arbitrary; what matters is that PINNED_NOW - 7200 (the raw->T1
 * threshold) is an exact multiple of 5, so the bucket ends exactly on the
 * cutoff. That is the only bucket the two statements could disagree on. */

#define PINNED_NOW 1800000000L
#define T1_BUCKET 5
#define T1_THRESHOLD 7200L

static int consolidate_at(db_t *db, long now)
{
    if (build_consolidate_sql(db->sql_consolidate, CONSOLIDATE_SQL_SIZE, now) != 0)
        return -1;
    return sqlite3_exec(db->handle, db->sql_consolidate, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int seed_boundary_bucket(db_t *db, long now)
{
    long bucket_start = now - T1_THRESHOLD - T1_BUCKET;
    for (int i = 0; i < T1_BUCKET; i++)
        if (insert_raw_row(db->handle, bucket_start + i) != SQLITE_OK)
            return -1;
    return 0;
}

static int test_consolidate_sql_reads_no_clock(void)
{
    char sql[CONSOLIDATE_SQL_SIZE];
    if (build_consolidate_sql(sql, sizeof(sql), PINNED_NOW) != 0)
        return 1;
    return strstr(sql, "'now'") == NULL ? 0 : 1;
}

static int test_consolidate_boundary_bucket_is_aggregated(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    if (seed_boundary_bucket(&db, PINNED_NOW) != 0 || consolidate_at(&db, PINNED_NOW) != 0) {
        close_test_db(&db);
        return 1;
    }

    int raw = count_rows(db.handle, "bucket_sec IS NULL");
    int agg = count_rows(db.handle, "bucket_sec = 5");
    close_test_db(&db);

    return (raw == 0 && agg == 1) ? 0 : 1;
}

/* One second earlier the bucket is not due yet, so nothing may move. This is
 * the cutoff the INSERT used to see while the DELETE had already moved on. */
static int test_consolidate_boundary_bucket_not_due(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    if (seed_boundary_bucket(&db, PINNED_NOW) != 0 || consolidate_at(&db, PINNED_NOW - 1) != 0) {
        close_test_db(&db);
        return 1;
    }

    int raw = count_rows(db.handle, "bucket_sec IS NULL");
    int agg = count_rows(db.handle, "bucket_sec = 5");
    close_test_db(&db);

    return (raw == T1_BUCKET && agg == 0) ? 0 : 1;
}

static const test_t ALL_TESTS[] = {
    /* db_open validate-on-open state machine */
    T(db_open_fresh_install),
    T(db_open_existing_v02_ok),
    T(db_open_unmigrated_v01),
    T(db_open_foreign_app_id),
    T(db_open_outdated_minimoni),
    T(db_open_pragmas_unchanged_on_refuse),
    T(db_open_corrupt_file),
    T(db_open_directory_as_path),
    T(db_open_reopen_after_fresh),
    T(db_open_path_in_nonexistent_dir),
    /* db_insert and query round-trip */
    T(db_insert_roundtrip),
    T(db_insert_null_gating),
    T(db_query_range_bucketed_avg),
    /* consolidation: positive cases */
    T(consolidate_basic),
    T(consolidate_multiple_buckets),
    T(consolidate_idempotent),
    T(consolidate_recent_bucket_skipped),
    T(consolidate_rows_queryable),
    /* bucket-end predicate, one per tier transition */
    T(consolidate_bucket_straddles_threshold),
    T(consolidate_straddles_raw_t1),
    T(consolidate_straddles_t1_t2),
    T(consolidate_straddles_t3_t4),
    T(consolidate_straddles_t4_t5),
    /* one cutoff instant for the whole pass */
    T(consolidate_sql_reads_no_clock),
    T(consolidate_boundary_bucket_is_aggregated),
    T(consolidate_boundary_bucket_not_due),
};

int main(void)
{
    /* Silence db_open's diagnostics so the test output stays readable. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
