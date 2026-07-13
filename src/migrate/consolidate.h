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

#ifndef MIGRATE_CONSOLIDATE_H
#define MIGRATE_CONSOLIDATE_H

/* Fold the whole tier backlog to convergence and VACUUM, crossing every
 * statement through `minimoni db exec` (migrate carries no SQLite). Closing
 * step of a v0.1 -> v0.2 migration. Returns 0 on success, nonzero on failure
 * (reason already printed to stderr). */
int migrate_consolidate_and_vacuum(const char *minimoni_exec, const char *db_path);

#endif /* MIGRATE_CONSOLIDATE_H */
