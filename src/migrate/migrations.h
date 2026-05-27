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

#ifndef MINIMONI_MIGRATE_MIGRATIONS_H
#define MINIMONI_MIGRATE_MIGRATIONS_H

#include <stddef.h>

/*
 * One migration step in the user_version chain.
 *
 * `script` is a complete multi-statement SQL string passed verbatim to
 * `minimoni db exec`. It MUST start with `BEGIN` and end with `COMMIT`
 * so the whole migration is atomic: any failure rolls back to the
 * previous user_version. The final statement before COMMIT must bump
 * `PRAGMA user_version` to `to_version`.
 *
 * `verify_source` (optional, may be NULL) is invoked before any write.
 * It runs a schema fingerprint check appropriate for the source
 * version — typically reads `sqlite_master` and compares it against
 * the canonical schema as the daemon of that version would have
 * created it. Returns 0 on match, non-zero on any deviation (which
 * causes minimoni-migrate to refuse without writing). The function
 * is responsible for printing the failure reason to stderr.
 */
typedef struct {
    int from_version;
    int to_version;
    int (*verify_source)(const char *minimoni_exec, const char *db_path);
    const char *script;
} migration_t;

extern const migration_t MIGRATIONS[];
extern const size_t      NUM_MIGRATIONS;

/* Latest version known to this build of minimoni-migrate.
 * Derived from MIGRATIONS[NUM_MIGRATIONS-1].to_version. */
int migrations_latest_version(void);

/* Lookup a migration by its source version. Returns NULL if no migration
 * starts from `from`. */
const migration_t *migrations_find(int from);

#endif /* MINIMONI_MIGRATE_MIGRATIONS_H */
