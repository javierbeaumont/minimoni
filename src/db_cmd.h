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

#ifndef MINIMONI_DB_CMD_H
#define MINIMONI_DB_CMD_H

/*
 * Read-only inspection of a minimoni SQLite database. Prints:
 *  - file path and size on disk (+ WAL, SHM companions)
 *  - PRAGMA application_id / user_version / journal_mode / page_size
 *  - row count, oldest/newest timestamp, time span
 *  - row distribution per bucket_sec (tier)
 *  - alert_log row count and most recent alert
 *
 * The DB is opened with SQLITE_OPEN_READONLY — no PRAGMAs are written,
 * no statements are prepared. Safe to run against backup copies or
 * databases owned by another process (read permission permitting).
 *
 * Returns 0 on success, 1 on error (message written to stderr).
 */
int db_cmd_info(const char *db_path);

#endif /* MINIMONI_DB_CMD_H */
