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

#endif /* MINIMONI_DB_CMD_H */
