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

/* Integration tests for src/db_cmd.c against the real SQLite engine, in process. Run with: make
 * test-integration. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runner.h"

#include "../src/db_cmd.c"

static long count_rows(const char *path)
{
    sqlite3      *db = NULL;
    sqlite3_stmt *stmt = NULL;
    long          n = -1;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return n;
}

/* minimoni-migrate's backup runs VACUUM INTO while the daemon may hold the database open: the copy
 * has the rows that connection committed and SQLite has not yet moved out of its WAL. */
static int test_vacuum_into_copies_rows_still_in_a_wal(void)
{
    char src[64], wal[80], dst[64];
    snprintf(src, sizeof(src), "/tmp/minimoni-int-dbcmd-%d.db", getpid());
    snprintf(wal, sizeof(wal), "%s-wal", src);
    snprintf(dst, sizeof(dst), "/tmp/minimoni-int-dbcmd-%d-copy.db", getpid());
    unlink(src);
    unlink(dst);

    sqlite3    *writer = NULL;
    int         ok = sqlite3_open(src, &writer) == SQLITE_OK &&
                     sqlite3_exec(writer,
                                  "PRAGMA journal_mode=WAL; CREATE TABLE t (x); "
                                  "INSERT INTO t VALUES (1), (2);",
                                  NULL, NULL, NULL) == SQLITE_OK;
    struct stat st;
    ok = ok && stat(wal, &st) == 0 && st.st_size > 0;

    ok = ok && db_cmd_exec(src, "VACUUM INTO ?1", dst) == 0 && count_rows(dst) == 2;
    sqlite3_close(writer);
    unlink(src);
    unlink(dst);

    return ok ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(vacuum_into_copies_rows_still_in_a_wal),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
