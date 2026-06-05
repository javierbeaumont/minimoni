/*
 * minimoni — zero-dependency system monitoring
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
#include <time.h>

#include "db.h"

#define RETRY_COUNT 3
#define RETRY_DELAY_NS (100 * 1000000L) /* 100 ms */

/* --- SQL statements ------------------------------------------------------ */

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
    "  net_rx_bytes     INTEGER, net_tx_bytes INTEGER,"
    "  uptime_seconds   REAL"
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
                                 "  temp_celsius, net_rx_bytes, net_tx_bytes, uptime_seconds"
                                 ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

static const char SQL_PRUNE_METRICS[] = "DELETE FROM metrics WHERE timestamp < datetime('now', ?)";

static const char SQL_PRUNE_ALERTS[] = "DELETE FROM alert_log WHERE fired_at < datetime('now', ?)";

static const char SQL_ALERT_CHECK[] =
    "SELECT COUNT(*) FROM alert_log WHERE alert_name = ? AND fired_at > ?";

static const char SQL_ALERT_FIRE[] = "INSERT INTO alert_log (alert_name, fired_at) VALUES (?, ?)";

/* --- Helpers ------------------------------------------------------------- */

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

/* --- Public API ---------------------------------------------------------- */

int db_open(db_t *db, const char *path)
{
    memset(db, 0, sizeof(*db));

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
    sqlite3_exec(db->handle, "PRAGMA cache_size=-256", NULL, NULL, NULL);

    if (sqlite3_exec(db->handle, SQL_CREATE, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "db: schema error: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(db->handle);
        db->handle = NULL;
        return -1;
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

    sqlite3_bind_int64(s, 17, m->net_rx_bytes);
    sqlite3_bind_int64(s, 18, m->net_tx_bytes);
    sqlite3_bind_double(s, 19, m->uptime_seconds);

    int rc = step_with_retry(s);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: insert error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }
    return 0;
}

int db_prune(db_t *db, int retention_days)
{
    char offset[32];
    snprintf(offset, sizeof(offset), "-%d days", retention_days);

    sqlite3_reset(db->stmt_prune_metrics);
    sqlite3_bind_text(db->stmt_prune_metrics, 1, offset, -1, SQLITE_TRANSIENT);
    int rc = step_with_retry(db->stmt_prune_metrics);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: prune metrics error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_reset(db->stmt_prune_alerts);
    sqlite3_bind_text(db->stmt_prune_alerts, 1, offset, -1, SQLITE_TRANSIENT);
    rc = step_with_retry(db->stmt_prune_alerts);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db: prune alerts error: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }

    return 0;
}

/* --- Alert log ------------------------------------------------------------ */

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

/* --- Query helpers -------------------------------------------------------- */

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

/* --- db_current ----------------------------------------------------------- */

int db_current(db_t *db, db_row_t *row)
{
    /* Two most recent rows ordered newest-first; columns mirror the SELECT
     * list used by db_query_range so the same read_row() helper applies,
     * except columns 17/18 here are raw cumulative bytes (not rates). */
    static const char SQL[] = "SELECT timestamp,"
                              "  CAST(strftime('%s',timestamp) AS INTEGER),"
                              "  load_1m, load_5m, load_15m,"
                              "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
                              "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                              "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                              "  temp_celsius, net_rx_bytes, net_tx_bytes, uptime_seconds"
                              " FROM metrics ORDER BY timestamp DESC LIMIT 2";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->handle, SQL, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: current prepare: %s\n", sqlite3_errmsg(db->handle));
        return -1;
    }

    memset(row, 0, sizeof(*row));

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return 1; /* no rows yet */
    }

    /* Latest row — read all scalar fields directly */
    const char *ts = (const char *)sqlite3_column_text(s, 0);
    if (ts)
        snprintf(row->timestamp, sizeof(row->timestamp), "%s", ts);
    row->unix_time = (long)sqlite3_column_int64(s, 1);
    row->load_1m = sqlite3_column_double(s, 2);
    row->load_5m = sqlite3_column_double(s, 3);
    row->load_15m = sqlite3_column_double(s, 4);
    row->cpu_valid = (sqlite3_column_type(s, 5) != SQLITE_NULL);
    if (row->cpu_valid) {
        row->cpu_user_percent = sqlite3_column_double(s, 5);
        row->cpu_system_percent = sqlite3_column_double(s, 6);
        row->cpu_idle_percent = sqlite3_column_double(s, 7);
    }
    row->mem_total_mb = sqlite3_column_double(s, 8);
    row->mem_used_mb = sqlite3_column_double(s, 9);
    row->mem_available_mb = sqlite3_column_double(s, 10);
    row->mem_percent = sqlite3_column_double(s, 11);
    row->disk_total_gb = sqlite3_column_double(s, 12);
    row->disk_used_gb = sqlite3_column_double(s, 13);
    row->disk_free_gb = sqlite3_column_double(s, 14);
    row->disk_percent = sqlite3_column_double(s, 15);
    row->temp_valid = (sqlite3_column_type(s, 16) != SQLITE_NULL);
    if (row->temp_valid)
        row->temp_celsius = sqlite3_column_double(s, 16);

    long long cur_rx = sqlite3_column_int64(s, 17);
    long long cur_tx = sqlite3_column_int64(s, 18);
    row->uptime_seconds = sqlite3_column_double(s, 19);

    /* Previous row — only needed for net rate */
    if (sqlite3_step(s) == SQLITE_ROW) {
        long      prev_ts = (long)sqlite3_column_int64(s, 1);
        long long prev_rx = sqlite3_column_int64(s, 17);
        long long prev_tx = sqlite3_column_int64(s, 18);
        long      dt = row->unix_time - prev_ts;
        if (dt > 0) {
            long long drx = cur_rx - prev_rx;
            long long dtx = cur_tx - prev_tx;
            if (drx >= 0 && dtx >= 0) {
                row->net_valid = 1;
                row->net_rx_bps = (double)drx / (double)dt;
                row->net_tx_bps = (double)dtx / (double)dt;
            }
        }
    }

    sqlite3_finalize(s);
    return 0;
}

/* --- db_query_range ------------------------------------------------------- */

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
 * 19  uptime_seconds
 */

static const char SQL_RAW[] =
    "WITH d AS ("
    "  SELECT timestamp,"
    "    CAST(strftime('%s',timestamp) AS INTEGER) AS ts,"
    "    load_1m, load_5m, load_15m,"
    "    cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
    "    mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
    "    disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
    "    temp_celsius,"
    "    net_rx_bytes - LAG(net_rx_bytes) OVER (ORDER BY timestamp) AS rx_d,"
    "    net_tx_bytes - LAG(net_tx_bytes) OVER (ORDER BY timestamp) AS tx_d,"
    "    CAST(strftime('%s',timestamp) AS INTEGER)"
    "      - CAST(strftime('%s',LAG(timestamp) OVER (ORDER BY timestamp)) AS INTEGER) AS dt,"
    "    uptime_seconds"
    "  FROM metrics WHERE timestamp >= datetime('now',?)"
    ")"
    "SELECT timestamp, ts,"
    "  load_1m, load_5m, load_15m,"
    "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
    "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
    "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
    "  temp_celsius,"
    "  CASE WHEN rx_d>=0 AND dt>0 THEN CAST(rx_d AS REAL)/dt ELSE NULL END,"
    "  CASE WHEN tx_d>=0 AND dt>0 THEN CAST(tx_d AS REAL)/dt ELSE NULL END,"
    "  uptime_seconds"
    " FROM d ORDER BY ts";

int db_query_range(db_t *db, long range_seconds, int bucket_sec, db_row_t **out_rows)
{
    char offset[32];
    snprintf(offset, sizeof(offset), "-%ld seconds", range_seconds);

    sqlite3_stmt *s;

    if (bucket_sec <= 0) {
        if (sqlite3_prepare_v2(db->handle, SQL_RAW, -1, &s, NULL) != SQLITE_OK) {
            fprintf(stderr, "db: range prepare: %s\n", sqlite3_errmsg(db->handle));
            return -1;
        }
        sqlite3_bind_text(s, 1, offset, -1, SQLITE_TRANSIENT);
    } else {
        /* Build bucketed SQL with the bucket size embedded as a literal so
         * integer division in SQLite uses the correct type. */
        char sql[2048];
        snprintf(sql, sizeof(sql),
                 "WITH d AS ("
                 "  SELECT"
                 "    (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d AS bkt,"
                 "    load_1m, load_5m, load_15m,"
                 "    cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
                 "    mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                 "    disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                 "    temp_celsius,"
                 "    net_rx_bytes - LAG(net_rx_bytes) OVER (ORDER BY timestamp) AS rx_d,"
                 "    net_tx_bytes - LAG(net_tx_bytes) OVER (ORDER BY timestamp) AS tx_d,"
                 "    CAST(strftime('%%s',timestamp) AS INTEGER)"
                 "      - CAST(strftime('%%s',LAG(timestamp)"
                 "          OVER (ORDER BY timestamp)) AS INTEGER) AS dt,"
                 "    uptime_seconds"
                 "  FROM metrics WHERE timestamp >= datetime('now',?)"
                 ")"
                 "SELECT datetime(bkt,'unixepoch'), bkt,"
                 "  AVG(load_1m), AVG(load_5m), AVG(load_15m),"
                 "  AVG(cpu_user_percent), AVG(cpu_system_percent), AVG(cpu_idle_percent),"
                 "  AVG(mem_total_mb), AVG(mem_used_mb), AVG(mem_available_mb), AVG(mem_percent),"
                 "  AVG(disk_total_gb), AVG(disk_used_gb), AVG(disk_free_gb), AVG(disk_percent),"
                 "  AVG(temp_celsius),"
                 "  AVG(CASE WHEN rx_d>=0 AND dt>0 THEN CAST(rx_d AS REAL)/dt ELSE NULL END),"
                 "  AVG(CASE WHEN tx_d>=0 AND dt>0 THEN CAST(tx_d AS REAL)/dt ELSE NULL END),"
                 "  AVG(uptime_seconds)"
                 " FROM d GROUP BY bkt ORDER BY bkt",
                 bucket_sec, bucket_sec);
        if (sqlite3_prepare_v2(db->handle, sql, -1, &s, NULL) != SQLITE_OK) {
            fprintf(stderr, "db: range bucket prepare: %s\n", sqlite3_errmsg(db->handle));
            return -1;
        }
        sqlite3_bind_text(s, 1, offset, -1, SQLITE_TRANSIENT);
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
