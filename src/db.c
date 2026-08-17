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

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "db.h"

#define RETRY_COUNT 3
#define RETRY_DELAY_NS (100 * 1000000L) /* 100 ms */

/* --- SQL statements --- */

static const char SQL_CREATE[] =
    "CREATE TABLE IF NOT EXISTS metrics ("
    "  timestamp        TEXT NOT NULL,"
    "  load_1m          REAL, load_5m REAL, load_15m REAL,"
    "  cpu_user_percent REAL, cpu_system_percent REAL, cpu_idle_percent REAL,"
    "  mem_total_mb     REAL, mem_used_mb REAL,"
    "  mem_available_mb REAL, mem_percent REAL,"
    "  disk_total_gb    REAL, disk_used_gb REAL,"
    "  disk_free_gb     REAL, disk_percent REAL,"
    "  temp_celsius     REAL,"
    "  net_rx_bps       REAL, net_tx_bps REAL,"
    "  uptime_seconds   REAL,"
    "  bucket_sec       INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(timestamp);"
    "CREATE TABLE IF NOT EXISTS alert_log ("
    "  alert_name TEXT NOT NULL,"
    "  fired_at   TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_alert_log_name ON alert_log(alert_name);";

static const char SQL_INSERT[] = "INSERT INTO metrics ("
                                 "  timestamp, load_1m, load_5m, load_15m,"
                                 "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
                                 "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                                 "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                                 "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds,"
                                 "  bucket_sec"
                                 ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

static const char SQL_PRUNE_METRICS[] = "DELETE FROM metrics WHERE timestamp < ?";

static const char SQL_PRUNE_ALERTS[] = "DELETE FROM alert_log WHERE fired_at < ?";

static const char SQL_ALERT_CHECK[] =
    "SELECT COUNT(*) FROM alert_log WHERE alert_name = ? AND fired_at > ?";

static const char SQL_ALERT_FIRE[] = "INSERT INTO alert_log (alert_name, fired_at) VALUES (?, ?)";

/* --- Helpers --- */

static int step_with_retry(sqlite3_stmt *stmt)
{
    struct timespec delay = {0, RETRY_DELAY_NS};
    int             tries = RETRY_COUNT;
    int             rc;

    while (tries-- > 0) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_BUSY)
            return rc;
        nanosleep(&delay, NULL);
    }
    return rc;
}

/* Read a single-row integer-valued PRAGMA into *out. Returns 0 on success,
 * -1 if the prepare or step failed (in which case *out is untouched). */
static int read_pragma_long(sqlite3 *handle, const char *pragma_name, long *out)
{
    char query[64];
    snprintf(query, sizeof(query), "PRAGMA %s", pragma_name);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(handle, query, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = (long)sqlite3_column_int64(stmt, 0);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    return rc;
}

#define CONSOLIDATE_SQL_SIZE 8192

/* Forward declaration; definition lives next to db_consolidate. */
static int build_consolidate_sql(char *sql, size_t cap, long now);

/* --- Public API --- */

int db_open(db_t *db, const char *path, long interval_sec)
{
    memset(db, 0, sizeof(*db));
    db->interval_sec = interval_sec;

    /* Detect whether the file existed BEFORE sqlite3_open touches it.
     * sqlite3_open creates a fresh empty database if the path is missing,
     * which is the only case where the daemon is allowed to author the
     * schema and pragmas itself. Any pre-existing file must come from either
     * an in-version daemon (proceed) or a prior schema the operator needs to
     * migrate explicitly (refuse to start). */
    struct stat st;
    int         existed_before = (stat(path, &st) == 0);

    if (sqlite3_open(path, &db->handle) != SQLITE_OK) {
        fprintf(stderr, "db: cannot open %s: %s\n", path, sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        db->handle = NULL;
        return -1;
    }

    char *errmsg = NULL;
    if (sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "db: WAL pragma failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        errmsg = NULL;
    }
    /* 64 KB on both connections (below ~32 KB the read syscalls explode).
     * A floor with margin, not a minimum. */
    sqlite3_exec(db->handle, "PRAGMA cache_size=-64", NULL, NULL, NULL);

    if (existed_before) {
        /* Pre-existing file. Validate that we know how to talk to it BEFORE
         * preparing any statements against the (possibly outdated) schema.
         * Mutating the file with SQL_CREATE or writing the version pragmas
         * here would silently advance the recorded schema version past the
         * schema actually present, leaving the DB claiming a version whose
         * columns do not exist (every prepare would then fail). The version
         * pragmas are owned by minimoni-migrate; the daemon only reads them. */
        long app_id = 0;
        long version = 0;
        if (read_pragma_long(db->handle, "application_id", &app_id) != 0 ||
            read_pragma_long(db->handle, "user_version", &version) != 0) {
            fprintf(stderr, "db: could not read schema metadata from %s\n", path);
            sqlite3_close(db->handle);
            db->handle = NULL;
            return -1;
        }
        if (app_id != MINIMONI_APPLICATION_ID) {
            fprintf(stderr,
                    "db: %s is not a minimoni database (application_id=%ld, "
                    "expected %d); refusing to write\n",
                    path, app_id, MINIMONI_APPLICATION_ID);
            sqlite3_close(db->handle);
            db->handle = NULL;
            return -1;
        }
        if (version != MINIMONI_SCHEMA_VERSION) {
            fprintf(stderr,
                    "db: schema at user_version=%ld; this daemon needs "
                    "user_version=%d. Run minimoni-migrate before starting "
                    "the daemon.\n",
                    version, MINIMONI_SCHEMA_VERSION);
            sqlite3_close(db->handle);
            db->handle = NULL;
            return -1;
        }
        /* All checks passed; do not touch the schema or pragmas. */
    } else {
        /* Fresh install: file did not exist before sqlite3_open created it.
         * This is the only branch in which the daemon authors the schema and
         * the version pragmas. */
        if (sqlite3_exec(db->handle, SQL_CREATE, NULL, NULL, &errmsg) != SQLITE_OK) {
            fprintf(stderr, "db: schema error: %s\n", errmsg);
            sqlite3_free(errmsg);
            sqlite3_close(db->handle);
            db->handle = NULL;
            return -1;
        }
        char pragma[48];
        snprintf(pragma, sizeof(pragma), "PRAGMA application_id = %d", MINIMONI_APPLICATION_ID);
        sqlite3_exec(db->handle, pragma, NULL, NULL, NULL);
        snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d", MINIMONI_SCHEMA_VERSION);
        sqlite3_exec(db->handle, pragma, NULL, NULL, NULL);
    }

    if (sqlite3_prepare_v2(db->handle, SQL_INSERT, -1, &db->stmt_insert, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db->handle, SQL_PRUNE_METRICS, -1, &db->stmt_prune_metrics, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(db->handle, SQL_PRUNE_ALERTS, -1, &db->stmt_prune_alerts, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(db->handle, SQL_ALERT_CHECK, -1, &db->stmt_alert_check, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(db->handle, SQL_ALERT_FIRE, -1, &db->stmt_alert_fire, NULL) !=
            SQLITE_OK) {
        fprintf(stderr, "db: prepare error: %s\n", sqlite3_errmsg(db->handle));
        db_close(db);
        return -1;
    }

    db->sql_consolidate = malloc(CONSOLIDATE_SQL_SIZE);
    if (!db->sql_consolidate) {
        fprintf(stderr, "db: out of memory allocating the consolidate SQL buffer\n");
        db_close(db);
        return -1;
    }

    return 0;
}

int db_open_readonly(db_t *db, const char *path)
{
    memset(db, 0, sizeof(*db));

    if (sqlite3_open_v2(path, &db->handle, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: cannot open %s read-only: %s\n", path, sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        db->handle = NULL;
        return -1;
    }
    sqlite3_exec(db->handle, "PRAGMA cache_size=-64", NULL, NULL, NULL);
    return 0;
}

void db_close(db_t *db)
{
    if (!db->handle)
        return;
    sqlite3_finalize(db->stmt_insert);
    sqlite3_finalize(db->stmt_prune_metrics);
    sqlite3_finalize(db->stmt_prune_alerts);
    sqlite3_finalize(db->stmt_alert_check);
    sqlite3_finalize(db->stmt_alert_fire);
    free(db->sql_consolidate);
    sqlite3_close(db->handle);
    memset(db, 0, sizeof(*db));
}

int db_insert(db_t *db, const metrics_t *m)
{
    time_t    now = time(NULL);
    struct tm utc;
    char      ts[24];

    gmtime_r(&now, &utc);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

    sqlite3_stmt *s = db->stmt_insert;
    sqlite3_reset(s);

    sqlite3_bind_text(s, 1, ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s, 2, m->load_1m);
    sqlite3_bind_double(s, 3, m->load_5m);
    sqlite3_bind_double(s, 4, m->load_15m);

    if (m->cpu_valid) {
        sqlite3_bind_double(s, 5, m->cpu_user_percent);
        sqlite3_bind_double(s, 6, m->cpu_system_percent);
        sqlite3_bind_double(s, 7, m->cpu_idle_percent);
    } else {
        sqlite3_bind_null(s, 5);
        sqlite3_bind_null(s, 6);
        sqlite3_bind_null(s, 7);
    }

    sqlite3_bind_double(s, 8, m->mem_total_mb);
    sqlite3_bind_double(s, 9, m->mem_used_mb);
    sqlite3_bind_double(s, 10, m->mem_available_mb);
    sqlite3_bind_double(s, 11, m->mem_percent);
    sqlite3_bind_double(s, 12, m->disk_total_gb);
    sqlite3_bind_double(s, 13, m->disk_used_gb);
    sqlite3_bind_double(s, 14, m->disk_free_gb);
    sqlite3_bind_double(s, 15, m->disk_percent);

    if (m->temp_valid)
        sqlite3_bind_double(s, 16, m->temp_celsius);
    else
        sqlite3_bind_null(s, 16);

    if (m->net_valid) {
        sqlite3_bind_double(s, 17, m->net_rx_bps);
        sqlite3_bind_double(s, 18, m->net_tx_bps);
    } else {
        sqlite3_bind_null(s, 17);
        sqlite3_bind_null(s, 18);
    }

    sqlite3_bind_double(s, 19, m->uptime_seconds);
    sqlite3_bind_int64(s, 20, db->interval_sec);

    int rc = step_with_retry(s);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: insert error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }
    return 0;
}

/* Format the instant `seconds_ago` before now as the canonical ISO-8601 UTC
 * string the daemon stores ("YYYY-MM-DDTHH:MM:SSZ"). WHERE clauses comparing
 * against the `timestamp` column must use this exact format: SQLite's
 * datetime('now', ?) returns "YYYY-MM-DD HH:MM:SS" (space, no T/Z), which sorts
 * lexicographically BEFORE the stored T+Z values for the same instant, so the
 * comparison over-includes rows. */
static void iso_cutoff(long seconds_ago, char *out, size_t out_size)
{
    time_t    t = time(NULL) - seconds_ago;
    struct tm utc;
    gmtime_r(&t, &utc);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

int db_prune(db_t *db, int retention_days)
{
    char cutoff[24];
    iso_cutoff((long)retention_days * 86400L, cutoff, sizeof(cutoff));

    sqlite3_reset(db->stmt_prune_metrics);
    sqlite3_bind_text(db->stmt_prune_metrics, 1, cutoff, -1, SQLITE_TRANSIENT);
    int rc = step_with_retry(db->stmt_prune_metrics);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: prune metrics error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_reset(db->stmt_prune_alerts);
    sqlite3_bind_text(db->stmt_prune_alerts, 1, cutoff, -1, SQLITE_TRANSIENT);
    rc = step_with_retry(db->stmt_prune_alerts);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: prune alerts error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }

    return 0;
}

/* --- Alert log --- */

int db_alert_on_cooldown(db_t *db, const char *alert_name, long cooldown_seconds)
{
    time_t    cutoff = time(NULL) - cooldown_seconds;
    struct tm utc;
    char      cutoff_str[24];

    gmtime_r(&cutoff, &utc);
    strftime(cutoff_str, sizeof(cutoff_str), "%Y-%m-%dT%H:%M:%SZ", &utc);

    sqlite3_stmt *s = db->stmt_alert_check;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, alert_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, cutoff_str, -1, SQLITE_TRANSIENT);

    int rc = step_with_retry(s);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db: alert check error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }
    return sqlite3_column_int(s, 0) > 0 ? 1 : 0;
}

int db_alert_log_fire(db_t *db, const char *alert_name)
{
    time_t    now = time(NULL);
    struct tm utc;
    char      fired_at[24];

    gmtime_r(&now, &utc);
    strftime(fired_at, sizeof(fired_at), "%Y-%m-%dT%H:%M:%SZ", &utc);

    sqlite3_stmt *s = db->stmt_alert_fire;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, alert_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, fired_at, -1, SQLITE_TRANSIENT);

    int rc = step_with_retry(s);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: alert fire error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }
    return 0;
}

/* --- Query helpers --- */

/* Read one db_row_t from the current row of stmt (columns 0-19 as defined
 * by both SQL_CURRENT and the SELECT list in db_query_range). */
static void read_row(sqlite3_stmt *s, db_row_t *r)
{
    memset(r, 0, sizeof(*r));

    const char *ts = (const char *)sqlite3_column_text(s, 0);
    if (ts)
        snprintf(r->timestamp, sizeof(r->timestamp), "%s", ts);
    r->unix_time = (long)sqlite3_column_int64(s, 1);

    r->load_1m = sqlite3_column_double(s, 2);
    r->load_5m = sqlite3_column_double(s, 3);
    r->load_15m = sqlite3_column_double(s, 4);

    r->cpu_valid = (sqlite3_column_type(s, 5) != SQLITE_NULL);
    if (r->cpu_valid) {
        r->cpu_user_percent = sqlite3_column_double(s, 5);
        r->cpu_system_percent = sqlite3_column_double(s, 6);
        r->cpu_idle_percent = sqlite3_column_double(s, 7);
    }

    r->mem_total_mb = sqlite3_column_double(s, 8);
    r->mem_used_mb = sqlite3_column_double(s, 9);
    r->mem_available_mb = sqlite3_column_double(s, 10);
    r->mem_percent = sqlite3_column_double(s, 11);

    r->disk_total_gb = sqlite3_column_double(s, 12);
    r->disk_used_gb = sqlite3_column_double(s, 13);
    r->disk_free_gb = sqlite3_column_double(s, 14);
    r->disk_percent = sqlite3_column_double(s, 15);

    r->temp_valid = (sqlite3_column_type(s, 16) != SQLITE_NULL);
    if (r->temp_valid)
        r->temp_celsius = sqlite3_column_double(s, 16);

    r->net_valid = (sqlite3_column_type(s, 17) != SQLITE_NULL);
    if (r->net_valid) {
        r->net_rx_bps = sqlite3_column_double(s, 17);
        r->net_tx_bps = sqlite3_column_double(s, 18);
    }

    r->uptime_seconds = sqlite3_column_double(s, 19);
}

/* --- db_current --- */

int db_current(db_t *db, db_row_t *row)
{
    /* Single-row read; columns mirror the SELECT list used by db_query_range
     * so the same read_row() helper applies. Net rates are stored directly
     * (computed at collect time in metrics.c), so no LAG/delta is needed. */
    static const char SQL[] = "SELECT timestamp,"
                              "  CAST(strftime('%s',timestamp) AS INTEGER),"
                              "  load_1m, load_5m, load_15m,"
                              "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
                              "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                              "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                              "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds"
                              " FROM metrics ORDER BY timestamp DESC LIMIT 1";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->handle, SQL, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: current prepare: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return 1; /* no rows yet */
    }

    read_row(s, row);
    sqlite3_finalize(s);
    return 0;
}

/* --- db_count_range --- */

int db_count_range(db_t *db, long range_seconds)
{
    char cutoff[24];
    iso_cutoff(range_seconds, cutoff, sizeof(cutoff));

    static const char SQL[] = "SELECT COUNT(*) FROM metrics WHERE timestamp >= ?";
    sqlite3_stmt     *s;
    if (sqlite3_prepare_v2(db->handle, SQL, -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, cutoff, -1, SQLITE_TRANSIENT);

    int count = -1;
    if (sqlite3_step(s) == SQLITE_ROW)
        count = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return count;
}

/* --- db_query_range --- */

/* Shared column layout for both raw and bucketed queries:
 *  0  timestamp (TEXT)
 *  1  unix_time (INTEGER)
 *  2  load_1m   3 load_5m   4 load_15m
 *  5  cpu_user_percent (nullable)  6 cpu_system_percent  7 cpu_idle_percent
 *  8  mem_total_mb  9 mem_used_mb  10 mem_available_mb  11 mem_percent
 * 12  disk_total_gb 13 disk_used_gb 14 disk_free_gb 15 disk_percent
 * 16  temp_celsius (nullable)
 * 17  net_rx_bps   (nullable, bytes/s)
 * 18  net_tx_bps   (nullable, bytes/s)
 * 19  uptime_seconds */

static const char SQL_RAW[] = "SELECT timestamp,"
                              "  CAST(strftime('%s',timestamp) AS INTEGER) AS ts,"
                              "  load_1m, load_5m, load_15m,"
                              "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
                              "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                              "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                              "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds"
                              " FROM metrics WHERE timestamp >= ? ORDER BY ts";

void db_release_memory(db_t *db) { sqlite3_db_release_memory(db->handle); }

int db_query_range(db_t *db, long range_seconds, int bucket_sec, db_row_t **out_rows)
{
    char cutoff[24];
    iso_cutoff(range_seconds, cutoff, sizeof(cutoff));

    sqlite3_stmt *s;

    if (bucket_sec <= 0) {
        if (sqlite3_prepare_v2(db->handle, SQL_RAW, -1, &s, NULL) != SQLITE_OK) {
            fprintf(stderr, "db: range prepare: %s\n", sqlite3_errmsg(db->handle));
            return -1;
        }
        sqlite3_bind_text(s, 1, cutoff, -1, SQLITE_TRANSIENT);
    } else {
        /* Build bucketed SQL with the bucket size embedded as a literal so
         * integer division in SQLite uses the correct type. */
        char sql[2048];
        snprintf(sql, sizeof(sql),
                 "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ',bkt,'unixepoch'), bkt,"
                 "  AVG(load_1m), AVG(load_5m), AVG(load_15m),"
                 "  AVG(cpu_user_percent), AVG(cpu_system_percent), AVG(cpu_idle_percent),"
                 "  AVG(mem_total_mb), AVG(mem_used_mb), AVG(mem_available_mb), AVG(mem_percent),"
                 "  AVG(disk_total_gb), AVG(disk_used_gb), AVG(disk_free_gb), AVG(disk_percent),"
                 "  AVG(temp_celsius), AVG(net_rx_bps), AVG(net_tx_bps), AVG(uptime_seconds)"
                 " FROM ("
                 "  SELECT (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d AS bkt,"
                 "    load_1m, load_5m, load_15m,"
                 "    cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
                 "    mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                 "    disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                 "    temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds"
                 "  FROM metrics WHERE timestamp >= ?"
                 ") GROUP BY bkt ORDER BY bkt",
                 bucket_sec, bucket_sec);
        if (sqlite3_prepare_v2(db->handle, sql, -1, &s, NULL) != SQLITE_OK) {
            fprintf(stderr, "db: range bucket prepare: %s\n", sqlite3_errmsg(db->handle));
            return -1;
        }
        sqlite3_bind_text(s, 1, cutoff, -1, SQLITE_TRANSIENT);
    }

    /* Collect rows into a growing heap buffer. */
    size_t    cap = 512;
    size_t    cnt = 0;
    db_row_t *rows = malloc(cap * sizeof(db_row_t));
    if (!rows) {
        sqlite3_finalize(s);
        return -1;
    }

    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (cnt == cap) {
            cap *= 2;
            db_row_t *tmp = realloc(rows, cap * sizeof(db_row_t));
            if (!tmp) {
                free(rows);
                sqlite3_finalize(s);
                return -1;
            }
            rows = tmp;
        }
        read_row(s, &rows[cnt++]);
    }

    sqlite3_finalize(s);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: range query error: %s\n", sqlite3_errmsg(db->handle));
        free(rows);
        return -1;
    }

    *out_rows = rows;
    return (int)cnt;
}

/* --- db_consolidate (write-time tiered consolidation) --- */

/* Tier ladder. Each entry describes a transition: source rows with bucket_sec
 * NULL or strictly less than `bucket_sec` (a finer tier or still raw) whose
 * bucket boundary has aged past `threshold_sec` are grouped, averaged, and
 * re-inserted with the new bucket_sec.
 *
 * The ladder is fixed and aligned to a design point of 1440 max points per
 * query (see docs/adr/0005-tiered-consolidation.md). Tiers whose bucket_sec is
 * <= the user's collect interval are inert: the `bucket_sec < N` predicate
 * matches no rows (raw rows carry bucket_sec = interval), so those passes are
 * no-ops at runtime.
 *
 * The WHERE clause operates on the bucket BOUNDARY (bucket_end <= now - X), NOT
 * on each row's timestamp. A row-level predicate would fire as each raw row
 * crosses the threshold, producing one duplicate consolidated row per collect
 * cycle inside the same bucket window. The bucket-level predicate guarantees
 * that all rows in a bucket qualify together or not at all.
 *
 * AVG() ignores NULLs, so cpu_user_percent / temp_celsius / net_rx_bps keep
 * their validity semantics. Consolidated rows are timestamped at the bucket
 * start in the same canonical T+Z format as raw rows (see iso_cutoff), so
 * range queries compare them correctly. */

struct tier_transition {
    int  bucket_sec;    /* destination bucket size (seconds) */
    long threshold_sec; /* consolidate when bucket_end <= now - this */
};

static const struct tier_transition TIER_TRANSITIONS[] = {
    {5, 2L * 3600},        /* Raw -> T1: 5s buckets,  threshold 2h */
    {30, 12L * 3600},      /* T1  -> T2: 30s buckets, threshold 12h */
    {300, 5L * 86400},     /* T2  -> T3: 5m buckets,  threshold 5d */
    {3600, 60L * 86400},   /* T3  -> T4: 1h buckets,  threshold 60d */
    {21600, 365L * 86400}, /* T4  -> T5: 6h buckets,  threshold 365d */
};
#define NUM_TIER_TRANSITIONS (sizeof(TIER_TRANSITIONS) / sizeof(TIER_TRANSITIONS[0]))

/* Build the consolidate SQL into `sql`: BEGIN IMMEDIATE; (5 x INSERT/DELETE);
 * COMMIT; Returns 0, or -1 if it does not fit.
 *
 * `now` is a parameter rather than something the statements read for
 * themselves: SQLite's 'now' is stable per statement, not per transaction, so
 * the INSERT and the DELETE of a tier can land on different seconds, and a
 * bucket between the two is deleted without having been aggregated. The INSERT
 * is the slow one (full GROUP BY scan). db_prune takes its cutoff in C for the
 * same reason. */
static int build_consolidate_sql(char *sql, size_t cap, long now)
{
    int   remaining = (int)cap;
    char *p = sql;
    int   n;

    n = snprintf(p, remaining, "BEGIN IMMEDIATE;");
    if (n < 0 || n >= remaining)
        return -1;
    p += n;
    remaining -= n;

    for (size_t i = 0; i < NUM_TIER_TRANSITIONS; i++) {
        int  bs = TIER_TRANSITIONS[i].bucket_sec;
        long cut = now - TIER_TRANSITIONS[i].threshold_sec;

        n = snprintf(
            p, remaining,
            "INSERT INTO metrics ("
            "  timestamp, load_1m, load_5m, load_15m,"
            "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
            "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
            "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
            "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds, bucket_sec"
            ") SELECT"
            "  strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ',"
            "           (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d, 'unixepoch'),"
            "  AVG(load_1m), AVG(load_5m), AVG(load_15m),"
            "  AVG(cpu_user_percent), AVG(cpu_system_percent), AVG(cpu_idle_percent),"
            "  AVG(mem_total_mb), AVG(mem_used_mb), AVG(mem_available_mb), AVG(mem_percent),"
            "  AVG(disk_total_gb), AVG(disk_used_gb), AVG(disk_free_gb), AVG(disk_percent),"
            "  AVG(temp_celsius), AVG(net_rx_bps), AVG(net_tx_bps), AVG(uptime_seconds),"
            "  %d"
            " FROM metrics"
            " WHERE (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d + %d <= %ld"
            "   AND (bucket_sec IS NULL OR bucket_sec < %d)"
            " GROUP BY (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d;"
            "DELETE FROM metrics"
            " WHERE (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d + %d <= %ld"
            "   AND (bucket_sec IS NULL OR bucket_sec < %d);",
            bs, bs,               /* SELECT strftime bucket expression */
            bs,                   /* INSERT bucket_sec value */
            bs, bs, bs, cut, bs,  /* INSERT WHERE */
            bs, bs,               /* GROUP BY */
            bs, bs, bs, cut, bs); /* DELETE WHERE */
        if (n < 0 || n >= remaining)
            return -1;
        p += n;
        remaining -= n;
    }

    n = snprintf(p, remaining, "COMMIT;");
    if (n < 0 || n >= remaining)
        return -1;

    return 0;
}

int db_consolidate(db_t *db)
{
    if (!db->sql_consolidate)
        return -1;
    if (build_consolidate_sql(db->sql_consolidate, CONSOLIDATE_SQL_SIZE, (long)time(NULL)) != 0) {
        fprintf(stderr, "db: consolidate SQL does not fit in %d bytes\n", CONSOLIDATE_SQL_SIZE);
        return -1;
    }
    char *errmsg = NULL;
    if (sqlite3_exec(db->handle, db->sql_consolidate, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "db: consolidate error: %s\n", errmsg ? errmsg : "(unknown)");
        sqlite3_free(errmsg);
        /* Best-effort rollback if BEGIN succeeded but a later statement failed
         * before COMMIT. */
        sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    /* No db_release_memory() here: cache_size bounds the page cache and SQLite trims within it. */
    return 0;
}
