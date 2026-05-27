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

#ifndef MINIMONI_MIGRATE_EXEC_H
#define MINIMONI_MIGRATE_EXEC_H

#include <stddef.h>

/*
 * Invoke `<minimoni_exec> db exec <db_path> <sql>` as a child process.
 * SQL is passed as a single argv element (no shell), so no quoting is
 * needed.
 *
 * stdout and stderr from the child are captured into the caller-supplied
 * buffers. Buffers are always NUL-terminated. If the child output exceeds
 * the buffer it is truncated; truncation is not signalled (use buffers
 * sized for the expected query — preflight queries are small).
 *
 * Returns the child's exit status (0/1/2 per `minimoni db exec`), or:
 *   -1 fork failed
 *   -2 exec failed (binary not found, not executable, etc.)
 *   -3 child crashed (signal)
 */
int migrate_exec(const char *minimoni_exec, const char *db_path, const char *sql, char *out_buf,
                 size_t out_size, char *err_buf, size_t err_size);

#endif /* MINIMONI_MIGRATE_EXEC_H */
