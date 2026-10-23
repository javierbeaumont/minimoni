/*
 * minimoni: zero-dependency system monitoring
 *
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This file is part of minimoni.
 *
 * minimoni is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * minimoni is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with minimoni. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MINIMONI_MIGRATE_SNAPSHOT_H
#define MINIMONI_MIGRATE_SNAPSHOT_H

/* Copy the database at src_path to dst_path through `minimoni db exec`'s VACUUM INTO: a consistent
 * copy, rows still in a WAL included, even while another connection writes. It holds the same data
 * in a compacted file. The copy replaces whatever was at dst, a symlink included (never what it
 * points to), along with any journal an earlier dst left, and is readable by its owner only. When
 * the copy fails, an earlier dst is left as it was. Returns 0 on success, non-zero on failure
 * (reason written to stderr). */
int migrate_snapshot(const char *minimoni_exec, const char *src_path, const char *dst_path);

#endif /* MINIMONI_MIGRATE_SNAPSHOT_H */
