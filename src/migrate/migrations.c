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

#include "migrations.h"

#include "exec.h"

#include <stdio.h>
#include <string.h>

/* --- v0 schema fingerprint (structural) -------------------------------- */

/* A canonical minimoni v0.1 database, described by STRUCTURE rather than by
 * the raw `CREATE TABLE` text. For each table (ordered by name) the query
 * emits its columns as `name:type:notnull` sorted by column NAME; for each
 * index it emits the target table and the indexed columns in index order.
 *
 * Sorting columns by name makes the fingerprint insensitive to physical
 * column ORDER and to `CREATE TABLE` whitespace. That is deliberate: a real
 * v0.1 database that grew its columns via `ALTER TABLE ADD COLUMN` (which
 * SQLite always appends at the end) is structurally identical to a
 * fresh-install schema and must migrate cleanly. This is sound only because
 * every migration script addresses columns by explicit name (no `SELECT *`,
 * no positional `INSERT`), so column order never affects the result.
 *
 * A genuine structural divergence (a column with a different type, an extra
 * or missing column/table/index) still fails the fingerprint and aborts
 * before any write or snapshot. The operator can override with `--force`
 * once they have reviewed the divergence. */
static const char V0_SCHEMA_QUERY[] =
    "SELECT 'table:' || sm.name || ':' || ("
    "  SELECT group_concat(ti.name || ':' || ti.type || ':' || ti.\"notnull\","
    "                      ',' ORDER BY ti.name)"
    "  FROM pragma_table_info(sm.name) ti)"
    " FROM sqlite_master sm WHERE sm.type = 'table' AND sm.name NOT LIKE 'sqlite_%'"
    " UNION ALL "
    "SELECT 'index:' || sm.name || ':' || sm.tbl_name || ':' || ("
    "  SELECT group_concat(ii.name, ',' ORDER BY ii.seqno)"
    "  FROM pragma_index_info(sm.name) ii)"
    " FROM sqlite_master sm WHERE sm.type = 'index' AND sm.name NOT LIKE 'sqlite_%'"
    " ORDER BY 1";

/* The structure V0_SCHEMA_QUERY produces for a canonical v0.1 database
 * (db exec prints one row per line, with a trailing newline). */
static const char V0_SCHEMA_FINGERPRINT[] =
    "index:idx_alert_log_name:alert_log:alert_name\n"
    "index:idx_metrics_ts:metrics:timestamp\n"
    "table:alert_log:alert_name:TEXT:1,fired_at:TEXT:1\n"
    "table:metrics:cpu_idle_percent:REAL:0,cpu_system_percent:REAL:0,"
    "cpu_user_percent:REAL:0,disk_free_gb:REAL:0,disk_percent:REAL:0,"
    "disk_total_gb:REAL:0,disk_used_gb:REAL:0,load_15m:REAL:0,load_1m:REAL:0,"
    "load_5m:REAL:0,mem_available_mb:REAL:0,mem_percent:REAL:0,mem_total_mb:REAL:0,"
    "mem_used_mb:REAL:0,net_rx_bytes:INTEGER:0,net_tx_bytes:INTEGER:0,"
    "temp_celsius:REAL:0,timestamp:TEXT:1,uptime_seconds:REAL:0\n";

static int verify_v0_schema(const char *minimoni_exec, const char *db_path)
{
    char out[4096];
    char err[1024];
    int  rc =
        migrate_exec(minimoni_exec, db_path, V0_SCHEMA_QUERY, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "migrate: failed to read schema for fingerprint check: %s",
                err[0] ? err : "(no error message)\n");
        return 1;
    }
    if (strcmp(out, V0_SCHEMA_FINGERPRINT) != 0) {
        fprintf(stderr,
                "migrate: v0 schema fingerprint mismatch: this database is not "
                "structurally a canonical minimoni v0.1 schema, refusing.\n"
                "Review the divergence; re-run with --force to migrate anyway "
                "(a backup is always taken).\n"
                "--- expected ---\n%s"
                "--- got ---\n%s",
                V0_SCHEMA_FINGERPRINT, out);
        return 1;
    }
    return 0;
}

/* --- v0 -> v1 migration ------------------------------------------------ */

/* Schema changes carried by this migration:
 *
 *   - add `bucket_sec INTEGER` to `metrics` (tier marker for write-time
 *     consolidation; raw rows leave it NULL, consolidated rows write
 *     the bucket size in seconds, see ADR-0005)
 *   - add `net_rx_bps REAL` and `net_tx_bps REAL` to `metrics`; drop the
 *     v0.1 cumulative counters `net_rx_bytes INTEGER` and
 *     `net_tx_bytes INTEGER`. The v0.2 daemon stores rates directly.
 *   - bump `user_version` to 1. The moni application_id is already set
 *     (minimoni stamps it since v0.1), so the migration leaves it alone.
 *
 * Data conversions:
 *
 *   - Network rates are reconstructed from consecutive byte deltas
 *     divided by the timestamp delta between rows (the same arithmetic
 *     the v0.2 daemon performs at insert time). The very first row has
 *     no predecessor and ends up with NULL bps; accept that gap. Any
 *     counter reset (negative diff) or zero/negative time delta also
 *     produces NULL. */
static const char SCRIPT_V0_TO_V1[] =
    "BEGIN;"
    "ALTER TABLE metrics ADD COLUMN bucket_sec INTEGER;"
    "ALTER TABLE metrics ADD COLUMN net_rx_bps REAL;"
    "ALTER TABLE metrics ADD COLUMN net_tx_bps REAL;"
    "WITH ordered AS ("
    "  SELECT rowid,"
    "    net_rx_bytes - LAG(net_rx_bytes) OVER w AS rx_diff,"
    "    net_tx_bytes - LAG(net_tx_bytes) OVER w AS tx_diff,"
    "    strftime('%s', timestamp) - strftime('%s', LAG(timestamp) OVER w) AS dt"
    "  FROM metrics WINDOW w AS (ORDER BY timestamp)"
    ")"
    "UPDATE metrics SET"
    "  net_rx_bps = CASE"
    "    WHEN o.rx_diff IS NULL OR o.rx_diff < 0 OR o.dt IS NULL OR o.dt <= 0 THEN NULL"
    "    ELSE CAST(o.rx_diff AS REAL) / o.dt END,"
    "  net_tx_bps = CASE"
    "    WHEN o.tx_diff IS NULL OR o.tx_diff < 0 OR o.dt IS NULL OR o.dt <= 0 THEN NULL"
    "    ELSE CAST(o.tx_diff AS REAL) / o.dt END"
    "  FROM ordered o WHERE metrics.rowid = o.rowid;"
    "ALTER TABLE metrics DROP COLUMN net_rx_bytes;"
    "ALTER TABLE metrics DROP COLUMN net_tx_bytes;"
    "PRAGMA user_version = 1;"
    "COMMIT;";

/* --- Public table ------------------------------------------------------ */

const migration_t MIGRATIONS[] = {
    {0, 1, verify_v0_schema, SCRIPT_V0_TO_V1},
};

const size_t NUM_MIGRATIONS = sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]);

int migrations_latest_version(void)
{
    if (NUM_MIGRATIONS == 0)
        return 0;
    return MIGRATIONS[NUM_MIGRATIONS - 1].to_version;
}

const migration_t *migrations_find(int from)
{
    for (size_t i = 0; i < NUM_MIGRATIONS; i++) {
        if (MIGRATIONS[i].from_version == from)
            return &MIGRATIONS[i];
    }
    return NULL;
}
