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

#include "migrations.h"

#include "exec.h"

#include <stdio.h>
#include <string.h>

/* --- v0 schema fingerprint --------------------------------------------- */

/*
 * Exact concatenation of all sqlite_master.sql entries for a canonical
 * minimoni v0.1 database, ordered by (type, name) and joined with '\n'.
 *
 * This is the literal string the v0.1 daemon produced via its
 * `CREATE TABLE IF NOT EXISTS ...` statements (SQLite strips the
 * "IF NOT EXISTS" but preserves all other whitespace). Captured by
 * running the v0.1 SQL through `sqlite3` and querying:
 *
 *   SELECT sql FROM sqlite_master
 *    WHERE type IN ('table','index') AND name NOT LIKE 'sqlite_%'
 *    ORDER BY type, name;
 *
 * Any deviation — reordered columns, different types, extra tables,
 * renamed indexes — fails the fingerprint and aborts the migration.
 * Manual interventions on a v0.1 database (via `sqlite3` CLI etc.)
 * therefore force the user to either restore the canonical schema or
 * perform the migration by hand. This is intentional: we will not
 * silently transform data whose layout we have not verified.
 */
static const char V0_SCHEMA_FINGERPRINT[] =
    "CREATE INDEX idx_alert_log_name ON alert_log(alert_name)\n"
    "CREATE INDEX idx_metrics_ts ON metrics(timestamp)\n"
    "CREATE TABLE alert_log (  alert_name TEXT NOT NULL,  fired_at   TEXT NOT NULL)\n"
    "CREATE TABLE metrics (  timestamp        TEXT NOT NULL,  load_1m          REAL, "
    "load_5m REAL, load_15m REAL,  cpu_user_percent REAL, cpu_system_percent REAL, "
    "cpu_idle_percent REAL,  mem_total_mb     REAL, mem_used_mb REAL,  "
    "mem_available_mb REAL, mem_percent REAL,  disk_total_gb    REAL, disk_used_gb "
    "REAL,  disk_free_gb     REAL, disk_percent REAL,  temp_celsius     REAL,  "
    "net_rx_bytes     INTEGER, net_tx_bytes INTEGER,  uptime_seconds   REAL)\n";

static int verify_v0_schema(const char *minimoni_exec, const char *db_path)
{
    char out[4096];
    char err[1024];
    int  rc = migrate_exec(minimoni_exec, db_path,
                           "SELECT sql FROM sqlite_master"
                            " WHERE type IN ('table','index')"
                            "   AND name NOT LIKE 'sqlite_%'"
                            " ORDER BY type, name",
                           out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "migrate: failed to read schema for fingerprint check: %s",
                err[0] ? err : "(no error message)\n");
        return 1;
    }
    if (strcmp(out, V0_SCHEMA_FINGERPRINT) != 0) {
        fprintf(stderr,
                "migrate: v0 schema fingerprint mismatch — this database does not "
                "match a canonical minimoni v0.1 schema, refusing.\n"
                "--- expected ---\n%s"
                "--- got ---\n%s",
                V0_SCHEMA_FINGERPRINT, out);
        return 1;
    }
    return 0;
}

/* --- v0 -> v1 migration ------------------------------------------------ */

/*
 * Schema changes carried by this migration:
 *
 *   - add `bucket_sec INTEGER` to `metrics` (tier marker for write-time
 *     consolidation; raw rows leave it NULL, consolidated rows write
 *     the bucket size in seconds — see ADR-0005)
 *   - add `net_rx_bps REAL` and `net_tx_bps REAL` to `metrics`; drop the
 *     v0.1 cumulative counters `net_rx_bytes INTEGER` and
 *     `net_tx_bytes INTEGER`. The v0.2 daemon stores rates directly.
 *   - normalise existing `timestamp` strings from "YYYY-MM-DD HH:MM:SS"
 *     to "YYYY-MM-DDTHH:MM:SSZ", the canonical T+Z form the v0.2 daemon
 *     uses everywhere (lexicographic comparison against `datetime('now',?)`
 *     was the bug that drove this change — see also `db.c:iso_cutoff`).
 *   - tag the file as a minimoni database (`application_id = "moni"`
 *     magic) and bump `user_version` to 1.
 *
 * Data conversions:
 *
 *   - Network rates are reconstructed from consecutive byte deltas
 *     divided by the timestamp delta between rows (the same arithmetic
 *     the v0.2 daemon performs at insert time). The very first row has
 *     no predecessor and ends up with NULL bps — accept that gap. Any
 *     counter reset (negative diff) or zero/negative time delta also
 *     produces NULL.
 */
static const char SCRIPT_V0_TO_V1[] =
    "BEGIN;"
    "ALTER TABLE metrics ADD COLUMN bucket_sec INTEGER;"
    "ALTER TABLE metrics ADD COLUMN net_rx_bps REAL;"
    "ALTER TABLE metrics ADD COLUMN net_tx_bps REAL;"
    "UPDATE metrics"
    "  SET timestamp = strftime('%Y-%m-%dT%H:%M:%SZ', timestamp)"
    "  WHERE timestamp NOT LIKE '%T%';"
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
    "PRAGMA application_id = 1836019305;" /* 0x6D6F6E69 — "moni" */
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
