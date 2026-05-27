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

#ifndef MINIMONI_MIGRATE_SNAPSHOT_H
#define MINIMONI_MIGRATE_SNAPSHOT_H

/*
 * Copy src_path to dst_path, byte-for-byte. dst is created with 0600
 * (truncated if it exists). Returns 0 on success, non-zero on any I/O
 * error (error message written to stderr).
 *
 * Caveat: SQLite databases must NOT be written to during the copy or
 * the snapshot may capture an inconsistent page mix. The caller is
 * responsible for ensuring no writer is active — minimoni-migrate
 * assumes the daemon is stopped before the migration runs.
 */
int migrate_snapshot(const char *src_path, const char *dst_path);

#endif /* MINIMONI_MIGRATE_SNAPSHOT_H */
