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

#ifndef MINIMONI_DB_H
#define MINIMONI_DB_H

#include "metrics.h"
#include "sqlite3.h"

typedef struct {
    sqlite3      *handle;
    sqlite3_stmt *stmt_insert;
    sqlite3_stmt *stmt_prune_metrics;
    sqlite3_stmt *stmt_prune_alerts;
    sqlite3_stmt *stmt_alert_check; /* SELECT COUNT from alert_log by name + cutoff */
    sqlite3_stmt *stmt_alert_fire;  /* INSERT into alert_log */
} db_t;

/*
 * Open (or create) the database at path. Enables WAL mode, sets cache
 * to 256 KB, creates the schema, and prepares reusable statements.
 * Returns 0 on success, -1 on error (message written to stderr).
 */
int db_open(db_t *db, const char *path);

/* Finalize prepared statements and close the database handle. */
void db_close(db_t *db);

/*
 * Insert one metrics row timestamped at the current UTC second.
 * Fields gated by cpu_valid and temp_valid are stored as NULL when
 * invalid so they do not distort averages in downsampled queries.
 * Retries up to 3 times on SQLITE_BUSY with 100ms backoff.
 * Returns 0 on success, -1 on error.
 */
int db_insert(db_t *db, const metrics_t *m);

/*
 * Delete rows older than retention_days from metrics and alert_log.
 * Returns 0 on success, -1 on error.
 */
int db_prune(db_t *db, int retention_days);

/* -------------------------------------------------------------------------
 * Query API
 * ---------------------------------------------------------------------- */

/* One row of metrics, either from the latest snapshot or from a range query. */
typedef struct {
    char timestamp[24];
    long unix_time;
    /* load (always valid) */
    double load_1m, load_5m, load_15m;
    /* cpu (cpu_valid=0 on the first collect cycle — no previous snapshot) */
    int    cpu_valid;
    double cpu_user_percent, cpu_system_percent, cpu_idle_percent;
    /* memory */
    double mem_total_mb, mem_used_mb, mem_available_mb, mem_percent;
    /* disk */
    double disk_total_gb, disk_used_gb, disk_free_gb, disk_percent;
    /* temperature (temp_valid=0 when sensor is absent) */
    int    temp_valid;
    double temp_celsius;
    /* net throughput in bytes/s (net_valid=0 on the first row or after a
     * counter reset — the dashboard shows a gap instead of a spike) */
    int    net_valid;
    double net_rx_bps;
    double net_tx_bps;
    /* uptime */
    double uptime_seconds;
} db_row_t;

/*
 * Fetch the most recent metrics row into *row.  Net throughput is computed
 * from the two most recent rows; net_valid=0 when only one row exists or
 * when the counter rolled over.
 * Returns 0 on success, 1 when the table is empty, -1 on error.
 */
int db_current(db_t *db, db_row_t *row);

/*
 * Query time-series data for the past range_seconds seconds.
 * bucket_sec=0  → return raw rows (no aggregation).
 * bucket_sec>0  → aggregate into buckets of that size (AVG per bucket).
 * Net throughput is computed via LAG() and exposed as bytes/s; negative
 * deltas (counter reset) become net_valid=0 in the returned rows.
 *
 * On success allocates *out_rows on the heap (caller must free) and returns
 * the row count (>= 0).  Returns -1 on error.
 */
int db_query_range(db_t *db, long range_seconds, int bucket_sec, db_row_t **out_rows);

/* -------------------------------------------------------------------------
 * Alert log
 * ---------------------------------------------------------------------- */

/*
 * Check whether alert_name fired within the last cooldown_seconds.
 * Returns 0 if cooled down (safe to fire), 1 if still on cooldown,
 * -1 on database error.
 */
int db_alert_on_cooldown(db_t *db, const char *alert_name, long cooldown_seconds);

/*
 * Record a fire event for alert_name in alert_log with the current UTC time.
 * Returns 0 on success, -1 on error.
 */
int db_alert_log_fire(db_t *db, const char *alert_name);

#endif /* MINIMONI_DB_H */
