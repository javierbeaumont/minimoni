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

#ifndef MINIMONI_MIGRATE_PREFLIGHT_H
#define MINIMONI_MIGRATE_PREFLIGHT_H

/*
 * Read-only checks that gate every migration:
 *   - PRAGMA integrity_check returns "ok"
 *   - PRAGMA application_id is either 0 (unset, pre-v0.2) or the moni
 *     magic 0x6D6F6E69 (1836019305 decimal). Any other value means the
 *     file is a SQLite database belonging to a different application.
 *   - PRAGMA user_version is read and returned via *out_version. The
 *     caller decides whether the value is a valid migration source.
 *   - The `metrics` table exists with its baseline columns (`timestamp`,
 *     at least one of the metric columns). Catches "this is a SQLite
 *     file but not a minimoni metrics DB" earlier than the migration
 *     SQL would.
 *
 * On any failure, a human-readable explanation is printed to stderr.
 *
 * Returns 0 on pass (and writes user_version to *out_version), non-zero
 * on any failure (out_version is left untouched).
 */
int migrate_preflight(const char *minimoni_exec, const char *db_path, int *out_version);

#endif /* MINIMONI_MIGRATE_PREFLIGHT_H */
