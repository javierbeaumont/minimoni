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

#define _POSIX_C_SOURCE 200809L

#include "preflight.h"
#include "exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MONI_APPLICATION_ID 1836019305L /* 0x6D6F6E69 */

/* Trim a trailing newline (only one, since these are single-row outputs). */
static void chomp(char *s)
{
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n')
        s[n - 1] = '\0';
}

/* Run a single read-only query via `minimoni db exec` and copy the result
 * into `out`. Returns the exit code from migrate_exec (0 on SQL success).
 * On non-zero, the corresponding stderr is forwarded to our stderr so the
 * user sees the SQLite error verbatim. */
static int read_scalar(const char *minimoni_exec, const char *db_path, const char *sql, char *out,
                       size_t out_size)
{
    char err[512];
    int  rc = migrate_exec(minimoni_exec, db_path, sql, out, out_size, err, sizeof(err));
    if (rc == 0) {
        chomp(out);
        return 0;
    }
    if (rc < 0) {
        fprintf(stderr, "migrate: failed to invoke '%s': ", minimoni_exec);
        if (rc == -1)
            fprintf(stderr, "fork failed\n");
        else if (rc == -2)
            fprintf(stderr, "exec failed (binary not found or not executable)\n");
        else
            fprintf(stderr, "child terminated abnormally\n");
        return rc;
    }
    /* rc > 0: minimoni db exec exit code 1 (SQL error) or 2 (open error) */
    if (err[0])
        fputs(err, stderr);
    return rc;
}

int migrate_preflight(const char *minimoni_exec, const char *db_path, int *out_version)
{
    char buf[1024];

    /* integrity_check */
    if (read_scalar(minimoni_exec, db_path, "PRAGMA integrity_check", buf, sizeof(buf)) != 0)
        return 1;
    if (strcmp(buf, "ok") != 0) {
        fprintf(stderr,
                "migrate: PRAGMA integrity_check failed (got '%s'); refusing to "
                "migrate a damaged database\n",
                buf);
        return 1;
    }

    /* application_id: accept 0 (legacy/unset) or moni magic. Reject any other
     * value — that file belongs to a different application. */
    if (read_scalar(minimoni_exec, db_path, "PRAGMA application_id", buf, sizeof(buf)) != 0)
        return 1;
    long app_id = strtol(buf, NULL, 10);
    if (app_id != 0 && app_id != MONI_APPLICATION_ID) {
        fprintf(stderr,
                "migrate: application_id=%ld is not minimoni (expected 0 or %ld); "
                "refusing\n",
                app_id, MONI_APPLICATION_ID);
        return 1;
    }

    /* metrics table exists with a `timestamp` column */
    if (read_scalar(minimoni_exec, db_path,
                    "SELECT COUNT(*) FROM pragma_table_info('metrics') "
                    "WHERE name='timestamp'",
                    buf, sizeof(buf)) != 0)
        return 1;
    if (strtol(buf, NULL, 10) != 1) {
        fprintf(stderr, "migrate: table 'metrics' or column 'timestamp' missing; this "
                        "does not look like a minimoni database\n");
        return 1;
    }

    /* user_version — return to caller, no validation here */
    if (read_scalar(minimoni_exec, db_path, "PRAGMA user_version", buf, sizeof(buf)) != 0)
        return 1;
    *out_version = (int)strtol(buf, NULL, 10);
    return 0;
}
