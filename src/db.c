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
    "  cpu_user_pct     REAL, cpu_system_pct REAL, cpu_idle_pct REAL,"
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
                                 "  cpu_user_pct, cpu_system_pct, cpu_idle_pct,"
                                 "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
                                 "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
                                 "  temp_celsius, net_rx_bytes, net_tx_bytes, uptime_seconds"
                                 ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

static const char SQL_PRUNE_METRICS[] = "DELETE FROM metrics WHERE timestamp < datetime('now', ?)";

static const char SQL_PRUNE_ALERTS[] = "DELETE FROM alert_log WHERE fired_at < datetime('now', ?)";

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
        sqlite3_bind_double(s, 5, m->cpu_user_pct);
        sqlite3_bind_double(s, 6, m->cpu_system_pct);
        sqlite3_bind_double(s, 7, m->cpu_idle_pct);
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
