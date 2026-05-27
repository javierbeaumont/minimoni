/*
 * minimoni — zero-dependency system monitoring
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * Unit tests — zero-dependency, no framework. Build with:
 *   make test
 *
 * Each module under test is `#include`d directly so static helpers are
 * exercisable. The runner walks an array of {name, fn} pairs, prints
 * pass/fail, and exits non-zero on any failure.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Pull the modules under test in directly. */
#include "../src/config.c"
#include "../src/db.c"
#include "../src/db_cmd.c"

/* --- Test infrastructure ------------------------------------------------- */

static char g_tmpcfg_path[256];

/* Write `toml` to a temp file, run config_load on it. Returns config_load's
 * return code. Leaves cfg in whatever state config_load left it. */
static int load_cfg(config_t *cfg, const char *toml)
{
    snprintf(g_tmpcfg_path, sizeof(g_tmpcfg_path), "/tmp/minimoni-test-%d.toml", getpid());
    FILE *f = fopen(g_tmpcfg_path, "w");
    if (!f)
        return -2;
    fputs(toml, f);
    fclose(f);

    config_defaults(cfg);
    int rc = config_load(cfg, g_tmpcfg_path);
    unlink(g_tmpcfg_path);
    return rc;
}

/* --- Interval: values --------------------------------------------------- */

static int test_interval_negative(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = -10\n") == -1 ? 0 : 1;
}

static int test_interval_zero(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = 0\n") == -1 ? 0 : 1;
}

static int test_interval_min_boundary(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 1\n") != 0)
        return 1;
    return cfg.interval_seconds == 1 ? 0 : 1;
}

static int test_interval_default_value(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 60\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

static int test_interval_max_boundary(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 ? 0 : 1;
}

static int test_interval_clamp(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 3601\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 ? 0 : 1;
}

static int test_interval_clamp_huge(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 99999\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 ? 0 : 1;
}

static int test_interval_missing(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/tmp/x.db\"\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

/* --- Interval: wrong types ---------------------------------------------- */

static int test_interval_legacy_string(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"1m\"\n") == -1 ? 0 : 1;
}

static int test_interval_string_digits(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"60\"\n") == -1 ? 0 : 1;
}

static int test_interval_string_bogus(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"abc\"\n") == -1 ? 0 : 1;
}

static int test_interval_string_empty(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"\"\n") == -1 ? 0 : 1;
}

static int test_interval_float(void)
{
    config_t cfg;
    /* TOML_FP64 is unhandled; falls through to default (60). */
    if (load_cfg(&cfg, "[collect]\ninterval = 60.5\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

static int test_interval_bool(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = true\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

static int test_interval_array(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = [60]\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

/* --- Ranges: valid ------------------------------------------------------ */

static int test_ranges_valid_natural(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1d\", \"7d\", \"30d\", \"90d\"]\n") != 0)
        return 1;
    if (cfg.range_count != 4)
        return 1;
    return strcmp(cfg.ranges[0], "1d") == 0 && strcmp(cfg.ranges[3], "90d") == 0 ? 0 : 1;
}

static int test_ranges_valid_minutes(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1m\", \"1h\", \"1d\"]\n") != 0)
        return 1;
    return cfg.range_count == 3 ? 0 : 1;
}

static int test_ranges_empty(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = []\n") != 0)
        return 1;
    /* Empty array: ignored, defaults remain */
    return cfg.range_count == 4 ? 0 : 1;
}

static int test_ranges_missing(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/tmp/x.db\"\n") != 0)
        return 1;
    return cfg.range_count == 4 ? 0 : 1;
}

/* --- Ranges: wrong types ----------------------------------------------- */

static int test_ranges_string_not_array(void)
{
    config_t cfg;
    /* String instead of array — silently ignored, defaults kept */
    if (load_cfg(&cfg, "[dashboard]\nranges = \"1d\"\n") != 0)
        return 1;
    return cfg.range_count == 4 ? 0 : 1;
}

static int test_ranges_int_array(void)
{
    config_t cfg;
    /* Ints get skipped (not strings); all skipped → abort */
    return load_cfg(&cfg, "[dashboard]\nranges = [1, 2, 3]\n") == -1 ? 0 : 1;
}

static int test_ranges_bool_array(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [true, false]\n") == -1 ? 0 : 1;
}

static int test_ranges_nested_array(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [[\"1d\"]]\n") == -1 ? 0 : 1;
}

/* --- Ranges: invented / edge units ------------------------------------- */

static int test_ranges_weeks_unit(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"5w\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_years_unit(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1y\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_uppercase_h(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1H\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_uppercase_d(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1D\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_extra_suffix(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"100ms\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_no_unit(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_no_number(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"d\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_empty_string(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_negative(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"-1d\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_with_space(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1 d\"]\n") == -1 ? 0 : 1;
}

/* --- Ranges: per-unit upper bounds (caps) ------------------------------ */

static int test_ranges_minutes_at_cap(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"120m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_ranges_minutes_above_cap(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"121m\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_hours_at_cap(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"72h\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_ranges_hours_above_cap(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"73h\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_days_at_cap(void)
{
    config_t cfg;
    /* 3653d = max days in any 10-calendar-year window (3 leap years) */
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"3653d\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_ranges_days_above_cap(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"3654d\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_days_huge(void)
{
    config_t cfg;
    /* Typo where someone meant 3650d but added a zero — 36500d is ~100 years,
     * a plausible misconfiguration that should fail loud. */
    return load_cfg(&cfg, "[dashboard]\nranges = [\"36500d\"]\n") == -1 ? 0 : 1;
}

/* --- Combinations: interval + ranges ------------------------------------ */

static int test_combo_interval_eq_range_min(void)
{
    config_t cfg;
    /* interval=60 (= 1m), ranges=["1m"] → 1m equals interval, ok */
    if (load_cfg(&cfg, "[collect]\ninterval = 60\n[dashboard]\nranges = [\"1m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_eq_range_hour(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"1h\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_eq_range_60m(void)
{
    config_t cfg;
    /* 60m == 1h == 3600s — same as interval, ok */
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"60m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_skip_range(void)
{
    config_t cfg;
    /* 59m < interval (3600s), skipped, all skipped → abort */
    return load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"59m\"]\n") == -1
               ? 0
               : 1;
}

static int test_combo_partial_skip(void)
{
    config_t cfg;
    /* 5m < interval, skipped; 1h valid → 1 range */
    if (load_cfg(&cfg, "[collect]\ninterval = 600\n[dashboard]\nranges = [\"5m\", \"1h\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 && strcmp(cfg.ranges[0], "1h") == 0 ? 0 : 1;
}

static int test_combo_clamp_and_range(void)
{
    config_t cfg;
    /* interval clamped to 3600, then 1h range matches */
    if (load_cfg(&cfg, "[collect]\ninterval = 3601\n[dashboard]\nranges = [\"1h\"]\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 && cfg.range_count == 1 ? 0 : 1;
}

/* --- Order independence (retention = max regardless of position) ------- */

static int test_order_largest_first(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"90d\", \"1d\", \"7d\"]\n") != 0)
        return 1;
    /* All three valid, stored in given order */
    return cfg.range_count == 3 && strcmp(cfg.ranges[0], "90d") == 0 &&
                   strcmp(cfg.ranges[1], "1d") == 0 && strcmp(cfg.ranges[2], "7d") == 0
               ? 0
               : 1;
}

static int test_order_largest_middle(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1d\", \"90d\", \"7d\"]\n") != 0)
        return 1;
    return cfg.range_count == 3 && strcmp(cfg.ranges[1], "90d") == 0 ? 0 : 1;
}

/* --- Consolidation infrastructure --------------------------------------- */

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

/* Insert a raw metrics row with an explicit unix timestamp. All numeric
 * fields get plausible non-zero values so AVG() has data to work with. */
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
             "  datetime(%ld,'unixepoch'),"
             "  1.0,1.0,1.0,"
             "  50.0,5.0,45.0,"
             "  1000.0,500.0,500.0,50.0,"
             "  10.0,5.0,5.0,50.0,"
             "  42.0,100.0,200.0,1000.0"
             ");",
             unix_ts);
    return sqlite3_exec(h, sql, NULL, NULL, NULL);
}

/* Insert a row with explicit bucket_sec value, used by per-transition
 * regression tests to simulate "rows already in tier N" without going
 * through earlier consolidation passes. */
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
             "  datetime(%ld,'unixepoch'),"
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

/* --- Consolidation: positive cases -------------------------------------- */

static int test_consolidate_basic(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    /* 5 raw rows in a single bucket, 7 days ago — past the T2→T3 threshold
     * (5 d). At interval=60s the finer tiers (5s/30s) are inert, so the
     * first consolidation that fires is into 5-min buckets (bucket_sec=300). */
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

    /* 15 rows spanning 3 adjacent 5-min buckets, all 7 days ago (past
     * the T2→T3 threshold for interval=60s). */
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

    /* 5 raw rows, 7 days ago (past T2→T3 threshold). */
    long bucket = ((time(NULL) - 7 * 86400) / 300) * 300;
    for (int i = 0; i < 5; i++) {
        if (insert_raw_row(db.handle, bucket + i * 60) != SQLITE_OK) {
            close_test_db(&db);
            return 1;
        }
    }

    /* Five back-to-back consolidate cycles must not produce duplicate
     * medium rows. The bucket_sec < 300 predicate excludes already-
     * consolidated rows, so subsequent passes are no-ops. */
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

    /* 5 rows in a bucket from one hour ago — bucket_end is roughly 5 h
     * younger than the 6 h threshold, so the bucket is not yet eligible. */
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

/* Regression test for the row-level vs bucket-level predicate.
 *
 * Tests the T2→T3 transition (the first one that fires at interval=60s):
 * threshold = 5 days, destination bucket = 5 min (300 s).
 *
 * The 5-min bucket [bucket_start, bucket_start+300) is chosen so that it
 * strictly straddles (now - 5d): bucket_start <= now-5d < bucket_start+300.
 * Therefore bucket_end > now-5d and a correct bucket-end predicate keeps the
 * bucket as raw. A row-level predicate (timestamp < now-5d) would catch the
 * subset of rows in [bucket_start, now-5d) and produce a partial T3 row,
 * leaving fewer than 5 raw rows behind.
 *
 * Edge case: when (now - 5d) is exactly a multiple of 300 (roughly 0.3 % of
 * wall-clock seconds), bucket_start equals now-5d and no row satisfies the
 * row-level predicate either — both the correct and buggy implementations
 * leave all 5 rows raw. The test still passes; it just does not distinguish
 * the two. The other 99.7 % of the time, a regression to the row-level
 * predicate makes this assertion fail. */
static int test_consolidate_bucket_straddles_threshold(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;

    long X = time(NULL) - 5 * 86400;
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

/* Per-transition bucket-end predicate regression tests.
 *
 * Same shape as test_consolidate_bucket_straddles_threshold above, one per
 * remaining tier transition. Each inserts 5 rows in a destination-tier
 * bucket whose end strictly straddles the threshold age, with explicit
 * bucket_sec to isolate the targeted transition from cascading passes.
 *
 * Assertion in each: 0 rows promoted, 5 rows remain at the source tier.
 * A regression to a row-level predicate would catch part of the bucket and
 * produce a partial destination row.
 *
 * Same 0.3 % edge-case as the original (when threshold age is exactly a
 * multiple of the destination bucket, row-level and bucket-level give the
 * same answer — test passes vacuously). */

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

/* --- Mixed valid + invalid --------------------------------------------- */

static int test_mixed_some_invalid(void)
{
    config_t cfg;
    /* "bogus" skipped, others valid → 2 entries */
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1d\", \"bogus\", \"7d\"]\n") != 0)
        return 1;
    return cfg.range_count == 2 && strcmp(cfg.ranges[0], "1d") == 0 &&
                   strcmp(cfg.ranges[1], "7d") == 0
               ? 0
               : 1;
}

static int test_mixed_skip_and_valid(void)
{
    config_t cfg;
    /* 5m < interval 600 → skip; 1d valid → 1 entry */
    if (load_cfg(&cfg, "[collect]\ninterval = 600\n[dashboard]\nranges = [\"1d\", \"5m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 && strcmp(cfg.ranges[0], "1d") == 0 ? 0 : 1;
}

/* --- db_cmd_exec -------------------------------------------------------- */

/* Capture stdout/stderr produced by db_cmd_exec into caller-provided buffers.
 * Returns the exec exit code. Used by all db_exec tests.
 *
 * Stream redirection done with dup2 to temp files: pipes risk deadlock if
 * the writer fills the buffer and there is no reader. */
static int capture_exec(const char *db_path, const char *sql, char *out_buf, size_t out_size,
                        char *err_buf, size_t err_size)
{
    char tmpout[64], tmperr[64];
    snprintf(tmpout, sizeof(tmpout), "/tmp/minimoni-test-out-%d.txt", getpid());
    snprintf(tmperr, sizeof(tmperr), "/tmp/minimoni-test-err-%d.txt", getpid());

    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    int outfd = open(tmpout, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int errfd = open(tmperr, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    dup2(outfd, STDOUT_FILENO);
    dup2(errfd, STDERR_FILENO);
    close(outfd);
    close(errfd);

    int rc = db_cmd_exec(db_path, sql);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);

    out_buf[0] = '\0';
    err_buf[0] = '\0';
    FILE *fo = fopen(tmpout, "r");
    if (fo) {
        size_t n = fread(out_buf, 1, out_size - 1, fo);
        out_buf[n] = '\0';
        fclose(fo);
    }
    FILE *fe = fopen(tmperr, "r");
    if (fe) {
        size_t n = fread(err_buf, 1, err_size - 1, fe);
        err_buf[n] = '\0';
        fclose(fe);
    }
    unlink(tmpout);
    unlink(tmperr);
    return rc;
}

/* Simple SELECT: tab-separated, no header. */
static int test_exec_select(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    db_close(&db);

    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path, "SELECT 1, 'hi'", out, sizeof(out), err, sizeof(err));
    int  fail = rc != 0 || strcmp(out, "1\thi\n") != 0;
    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;
}

static int test_exec_null_rendering(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    db_close(&db);

    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path, "SELECT NULL", out, sizeof(out), err, sizeof(err));
    int  fail = rc != 0 || strcmp(out, "NULL\n") != 0;
    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;
}

static int test_exec_blob_rendering(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    db_close(&db);

    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path, "SELECT X'deadbeef'", out, sizeof(out), err, sizeof(err));
    int  fail = rc != 0 || strcmp(out, "X'DEADBEEF'\n") != 0;
    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;
}

/* Multi-statement DDL+DML succeeds end-to-end (the migration use case).
 * stdout stays empty because none of the statements return rows. */
static int test_exec_multi_stmt_script(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    db_close(&db);

    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path, "CREATE TABLE t(x INT); INSERT INTO t VALUES (1),(2),(3)",
                           out, sizeof(out), err, sizeof(err));
    int  fail = rc != 0 || out[0] != '\0' || err[0] != '\0';
    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;
}

static int test_exec_sql_error_returns_1(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    db_close(&db);

    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path, "SELECT * FROM nonexistent_table", out, sizeof(out), err,
                           sizeof(err));
    int  fail = rc != 1 || strstr(err, "statement 1") == NULL;
    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;
}

static int test_exec_open_error_returns_2(void)
{
    char out[1024], err[1024];
    int  rc =
        capture_exec("/nonexistent/path/to.db", "SELECT 1", out, sizeof(out), err, sizeof(err));
    return (rc == 2 && strstr(err, "cannot open") != NULL) ? 0 : 1;
}

static int test_exec_transaction_rollback(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    /* Create a table; insert a known row. */
    if (sqlite3_exec(db.handle, "CREATE TABLE t(x INT)", NULL, NULL, NULL) != SQLITE_OK)
        return 1;
    if (sqlite3_exec(db.handle, "INSERT INTO t VALUES (1)", NULL, NULL, NULL) != SQLITE_OK)
        return 1;
    db_close(&db);

    /* Try a multi-stmt that BEGINs, inserts, then errors. The COMMIT never
     * runs; the in-progress transaction must roll back when the connection
     * closes after the error. */
    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path,
                           "BEGIN; INSERT INTO t VALUES (99); SELECT * FROM nonexistent; COMMIT",
                           out, sizeof(out), err, sizeof(err));
    if (rc != 1)
        goto fail;

    /* Re-open to verify only the pre-existing row is present. */
    sqlite3 *h = NULL;
    if (sqlite3_open_v2(g_tmpdb_path, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(h, "SELECT x FROM t", -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(h);
        goto fail;
    }
    int seen = 0, bad = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        seen++;
        if (sqlite3_column_int(stmt, 0) != 1)
            bad = 1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(h);
    int fail = (seen != 1 || bad);

    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;

fail:;
    char wal2[280];
    snprintf(wal2, sizeof(wal2), "%s-wal", g_tmpdb_path);
    unlink(wal2);
    snprintf(wal2, sizeof(wal2), "%s-shm", g_tmpdb_path);
    unlink(wal2);
    unlink(g_tmpdb_path);
    return 1;
}

static int test_exec_empty_script(void)
{
    db_t db;
    if (open_test_db(&db) != 0)
        return 1;
    db_close(&db);

    char out[1024], err[1024];
    int  rc = capture_exec(g_tmpdb_path, "   \n\t  ", out, sizeof(out), err, sizeof(err));
    int  fail = rc != 0 || out[0] != '\0' || err[0] != '\0';
    char wal[280];
    snprintf(wal, sizeof(wal), "%s-wal", g_tmpdb_path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", g_tmpdb_path);
    unlink(wal);
    unlink(g_tmpdb_path);
    return fail;
}

/* --- Runner ------------------------------------------------------------ */

struct test {
    const char *name;
    int (*fn)(void);
};

#define T(n) {#n, test_##n}

static const struct test ALL_TESTS[] = {
    /* interval: values */
    T(interval_negative),
    T(interval_zero),
    T(interval_min_boundary),
    T(interval_default_value),
    T(interval_max_boundary),
    T(interval_clamp),
    T(interval_clamp_huge),
    T(interval_missing),
    /* interval: wrong types */
    T(interval_legacy_string),
    T(interval_string_digits),
    T(interval_string_bogus),
    T(interval_string_empty),
    T(interval_float),
    T(interval_bool),
    T(interval_array),
    /* ranges: valid */
    T(ranges_valid_natural),
    T(ranges_valid_minutes),
    T(ranges_empty),
    T(ranges_missing),
    /* ranges: wrong types */
    T(ranges_string_not_array),
    T(ranges_int_array),
    T(ranges_bool_array),
    T(ranges_nested_array),
    /* ranges: invented / edge units */
    T(ranges_weeks_unit),
    T(ranges_years_unit),
    T(ranges_uppercase_h),
    T(ranges_uppercase_d),
    T(ranges_extra_suffix),
    T(ranges_no_unit),
    T(ranges_no_number),
    T(ranges_empty_string),
    T(ranges_negative),
    T(ranges_with_space),
    /* ranges: per-unit caps */
    T(ranges_minutes_at_cap),
    T(ranges_minutes_above_cap),
    T(ranges_hours_at_cap),
    T(ranges_hours_above_cap),
    T(ranges_days_at_cap),
    T(ranges_days_above_cap),
    T(ranges_days_huge),
    /* combinations */
    T(combo_interval_eq_range_min),
    T(combo_interval_eq_range_hour),
    T(combo_interval_eq_range_60m),
    T(combo_interval_skip_range),
    T(combo_partial_skip),
    T(combo_clamp_and_range),
    /* order independence */
    T(order_largest_first),
    T(order_largest_middle),
    /* mixed */
    T(mixed_some_invalid),
    T(mixed_skip_and_valid),
    /* consolidation */
    T(consolidate_basic),
    T(consolidate_multiple_buckets),
    T(consolidate_idempotent),
    T(consolidate_recent_bucket_skipped),
    T(consolidate_bucket_straddles_threshold),
    /* per-transition bucket-end predicate regressions */
    T(consolidate_straddles_raw_t1),
    T(consolidate_straddles_t1_t2),
    T(consolidate_straddles_t3_t4),
    T(consolidate_straddles_t4_t5),
    /* db exec */
    T(exec_select),
    T(exec_null_rendering),
    T(exec_blob_rendering),
    T(exec_multi_stmt_script),
    T(exec_sql_error_returns_1),
    T(exec_open_error_returns_2),
    T(exec_transaction_rollback),
    T(exec_empty_script),
};

#define NUM_TESTS (sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]))

int main(void)
{
    /* Silence config_load's diagnostics during tests so the output stays
     * readable. Re-open later if a test needs to inspect stderr. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;

    int failed = 0;
    for (size_t i = 0; i < NUM_TESTS; i++) {
        printf("  %-35s ", ALL_TESTS[i].name);
        fflush(stdout);
        int r = ALL_TESTS[i].fn();
        if (r) {
            failed++;
            printf("FAIL\n");
        } else {
            printf("ok\n");
        }
    }

    printf("\n  %zu/%zu tests passed\n", NUM_TESTS - failed, NUM_TESTS);
    return failed ? 1 : 0;
}
