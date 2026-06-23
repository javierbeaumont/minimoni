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

#ifndef MINIMONI_DB_CMD_H
#define MINIMONI_DB_CMD_H

/* Read-only inspection of a minimoni SQLite database. Prints:
 *  - file path and size on disk (+ WAL, SHM companions)
 *  - format identifier: application_id + user_version ("moni v<N>", or a
 *    hex id for a non-minimoni file, in which case it stops there)
 *  - row count, oldest/newest timestamp, time span
 *  - per-tier row distribution (by bucket_sec)
 *  - alert_log row count and most recent alert
 *
 * SQLite-level internals (journal_mode, page_size) are deliberately not shown.
 * The DB is opened with SQLITE_OPEN_READONLY: nothing is written and the
 * daemon's prepared statements are never created, so it is safe to run against
 * backup copies or a database owned by a live daemon (read permission
 * permitting).
 *
 * Returns 0 on success, 1 on error (message written to stderr). */
int db_cmd_info(const char *db_path);

/* Execute a SQL script against the database (read-write). Stable API consumed
 * by minimoni-migrate. Intentionally minimal: the only consumer ships
 * hand-audited SQL constants and reads just the row data + exit code. There is
 * no allowlist, no sanitizer, no flags. For interactive SQL, use sqlite3.
 *
 * Accepts a multi-statement script (statements separated by ;), executed in
 * order, aborted at the first error.
 *
 * Output:
 *  - stdout: tab-separated values, one row per line. No header line. Rows from
 *    multiple statements are concatenated with no separator. NULL values are
 *    the literal "NULL"; BLOB columns render as X'<hex>' to avoid terminal
 *    corruption from opaque bytes.
 *  - stderr: SQL errors as "db exec: error at statement <N>: <msg>".
 *
 * Opened with SQLITE_OPEN_READWRITE: the file must already exist and be
 * writable. Loadable extensions are never enabled.
 *
 * Caveat: TEXT values are emitted raw; embedded tab/newline/control bytes break
 * TSV parsing. The minimoni schema only stores ISO-8601 strings and
 * config-provided alert names here, both controlled inputs.
 *
 * Returns 0 on full success, 1 on SQL error, 2 on argument/DB-open error. */
int db_cmd_exec(const char *db_path, const char *sql);

#endif /* MINIMONI_DB_CMD_H */
