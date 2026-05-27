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

#include "exec.h"
#include "migrations.h"
#include "preflight.h"
#include "snapshot.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [--no-backup] [--use PATH] <db_path>\n"
            "  %s --version\n"
            "  %s --help\n"
            "\n"
            "Options:\n"
            "  --no-backup   Skip snapshot before migrating (default: on).\n"
            "  --use PATH    Path to the minimoni binary. Default: search in\n"
            "                the same directory as this binary, then $PATH.\n",
            prog, prog, prog);
}

/* Resolve the path to the minimoni binary.
 *
 * Priority:
 *   1. --use PATH given on the command line
 *   2. <dirname of argv[0]>/minimoni — colocated install
 *   3. "minimoni" — found via $PATH at exec time
 *
 * For (2), we stat() the candidate; if it does not exist or is not
 * executable, we fall through to (3). Returned pointer is either the
 * user-provided `cli_value`, the static buffer `colocated`, or the
 * literal "minimoni". */
static const char *resolve_minimoni_exec(const char *argv0, const char *cli_value, char *colocated,
                                         size_t colocated_size)
{
    if (cli_value)
        return cli_value;

    /* dirname() may modify its argument and may return a pointer to a
     * static buffer; copy argv0 first. */
    char argv0_copy[1024];
    snprintf(argv0_copy, sizeof(argv0_copy), "%s", argv0);
    char *dir = dirname(argv0_copy);
    snprintf(colocated, colocated_size, "%s/minimoni", dir);

    struct stat st;
    if (stat(colocated, &st) == 0 && (st.st_mode & S_IXUSR))
        return colocated;

    return "minimoni"; /* let execvp find it on PATH */
}

static int do_migrate(const char *minimoni_exec, const char *db_path, int do_backup)
{
    int current = -1;
    if (migrate_preflight(minimoni_exec, db_path, &current) != 0)
        return 1; /* preflight already printed the reason */

    int latest = migrations_latest_version();
    fprintf(stderr, "migrate: database at user_version=%d, latest=%d\n", current, latest);

    if (current == latest) {
        fprintf(stderr, "migrate: already at the latest version, nothing to do\n");
        return 0;
    }
    if (current > latest) {
        fprintf(stderr,
                "migrate: database is at user_version=%d, but this build only "
                "knows up to %d; refusing\n",
                current, latest);
        return 1;
    }

    /* Verify the source schema fingerprint of the FIRST migration in the
     * chain BEFORE snapshotting. If the canonical schema doesn't match,
     * we have no business writing to (or even copying) this database.
     * Subsequent migrations are not pre-verified: their source schema is
     * whatever the previous migration's script produced — hand-audited C
     * we trust by construction. */
    {
        const migration_t *first = migrations_find(current);
        if (first && first->verify_source && first->verify_source(minimoni_exec, db_path) != 0)
            return 1; /* verify_source already printed the reason */
    }

    /* Snapshot once before applying any migration. The backup name includes
     * the source version so consecutive migrations don't overwrite each
     * other's safety nets. */
    if (do_backup) {
        char backup_path[1024];
        snprintf(backup_path, sizeof(backup_path), "%s.backup-pre-migrate-v%d", db_path, current);
        fprintf(stderr, "migrate: snapshotting to %s\n", backup_path);
        if (migrate_snapshot(db_path, backup_path) != 0)
            return 3;
    } else {
        fprintf(stderr, "migrate: --no-backup specified, skipping snapshot\n");
    }

    /* Apply migrations one by one until we reach `latest` or run out. */
    while (current < latest) {
        const migration_t *m = migrations_find(current);
        if (!m) {
            fprintf(stderr,
                    "migrate: no migration registered for user_version=%d; "
                    "this build cannot continue\n",
                    current);
            return 2;
        }

        fprintf(stderr, "migrate: applying v%d -> v%d\n", m->from_version, m->to_version);

        char out[256], err[1024];
        int  rc =
            migrate_exec(minimoni_exec, db_path, m->script, out, sizeof(out), err, sizeof(err));
        if (rc != 0) {
            fprintf(stderr, "migrate: v%d -> v%d failed (db exec rc=%d)\n", m->from_version,
                    m->to_version, rc);
            if (err[0])
                fputs(err, stderr);
            if (do_backup) {
                fprintf(stderr,
                        "migrate: transaction was rolled back. The snapshot at "
                        "%s.backup-pre-migrate-v%d is intact if you need to "
                        "restore manually.\n",
                        db_path, m->from_version);
            }
            return 2;
        }
        current = m->to_version;
    }

    fprintf(stderr, "migrate: success, database at user_version=%d\n", current);
    return 0;
}

int main(int argc, char **argv)
{
    int         do_backup = 1;
    const char *cli_use = NULL;
    const char *db_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("minimoni-migrate %s\n", MINIMONI_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--no-backup") == 0) {
            do_backup = 0;
            continue;
        }
        if (strcmp(argv[i], "--use") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "migrate: --use requires PATH\n");
                return 1;
            }
            cli_use = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "migrate: unknown flag '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        if (db_path) {
            fprintf(stderr, "migrate: unexpected argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        db_path = argv[i];
    }

    if (!db_path) {
        fprintf(stderr, "migrate: <db_path> required\n");
        usage(argv[0]);
        return 1;
    }

    char        colocated[1024];
    const char *minimoni_exec =
        resolve_minimoni_exec(argv[0], cli_use, colocated, sizeof(colocated));

    return do_migrate(minimoni_exec, db_path, do_backup);
}
