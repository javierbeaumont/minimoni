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

#endif /* MINIMONI_DB_H */
