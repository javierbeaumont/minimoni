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

#define _POSIX_C_SOURCE 200809L

#include "consolidate.h"
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
            "  %s [--no-backup | --force] [--use PATH] <db_path>\n"
            "  %s --dry-run [--use PATH] <db_path>\n"
            "  %s --version\n"
            "  %s --help\n"
            "\n"
            "Options:\n"
            "  --dry-run     Rehearse the migration on a throwaway copy of the\n"
            "                database; never touch the original. Prints a\n"
            "                'status:' line to stdout (up-to-date,\n"
            "                migration-pending, or blocked) and exits 0 when it\n"
            "                is safe to migrate, 1 otherwise.\n"
            "  --force       Skip the schema fingerprint check, for a schema you\n"
            "                have reviewed and know is safe to migrate. A backup\n"
            "                is mandatory, so this cannot be combined with\n"
            "                --no-backup.\n"
            "  --no-backup   Skip snapshot before migrating (default: on).\n"
            "  --use PATH    Path to the minimoni binary. Default: search in\n"
            "                the same directory as this binary, then $PATH.\n",
            prog, prog, prog, prog);
}

/* Resolve the path to the minimoni binary.
 *
 * Priority:
 *   1. --use PATH given on the command line
 *   2. <dirname of argv[0]>/minimoni: colocated install
 *   3. "minimoni": found via $PATH at exec time
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

/* Remove a temporary database file and its WAL/SHM sidecars, ignoring
 * any that do not exist. Used to clean up the dry-run scratch copy. */
static void remove_db_with_sidecars(const char *path)
{
    char sidecar[1100];
    unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    unlink(sidecar);
}

/* Run the migration chain.
 *
 * Normal mode (dry_run = 0): the light checks (preflight + source
 * fingerprint) run first and abort on any failure; then the migration is
 * applied to `db_path` itself, with a snapshot beforehand unless
 * do_backup is 0.
 *
 * Dry-run mode (dry_run = 1): the same light checks run, then the
 * migration is rehearsed on a throwaway copy of the database next to the
 * original (same filesystem, so disk-space conditions match the real
 * run). The original is never touched and no permanent snapshot is left.
 * A machine-readable `status:` line is printed to stdout; the exit code
 * is binary (0 = safe / nothing to do, 1 = would not apply). do_backup
 * is irrelevant in this mode. */
static int do_migrate(const char *minimoni_exec, const char *db_path, int do_backup, int dry_run,
                      int force)
{
    int current = -1;
    if (migrate_preflight(minimoni_exec, db_path, &current) != 0) {
        if (dry_run)
            printf("status: blocked\n");
        return 1; /* preflight already printed the reason */
    }

    int latest = migrations_latest_version();
    fprintf(stderr, "migrate: database at user_version=%d, latest=%d\n", current, latest);

    if (current == latest) {
        fprintf(stderr, "migrate: already at the latest version, nothing to do\n");
        if (dry_run)
            printf("status: up-to-date\n");
        return 0;
    }
    if (current > latest) {
        fprintf(stderr,
                "migrate: database is at user_version=%d, but this build only "
                "knows up to %d; refusing\n",
                current, latest);
        if (dry_run)
            printf("status: blocked\n");
        return 1;
    }

    /* Verify the source schema fingerprint of the FIRST migration in the
     * chain BEFORE snapshotting. If the canonical schema doesn't match, we
     * have no business writing to (or even copying) this database. Subsequent
     * migrations are not pre-verified: their source schema is whatever the
     * previous migration's script produced; hand-audited C we trust by
     * construction. This light check runs in both modes, unless --force tells
     * us the operator has reviewed a non-canonical schema and accepts it. */
    if (force) {
        fprintf(stderr, "migrate: --force given, skipping the schema fingerprint check\n");
    } else {
        const migration_t *first = migrations_find(current);
        if (first && first->verify_source && first->verify_source(minimoni_exec, db_path) != 0) {
            if (dry_run)
                printf("status: blocked\n");
            return 1; /* verify_source already printed the reason */
        }
    }

    /* Choose the target the migration script is applied to:
     *   - dry-run: a throwaway copy beside the original, discarded after.
     *   - normal:  the database itself, snapshotted first unless --no-backup. */
    const char *target = db_path;
    char        dry_path[1024];
    if (dry_run) {
        snprintf(dry_path, sizeof(dry_path), "%s.dry-run-%d", db_path, (int)getpid());
        remove_db_with_sidecars(dry_path); /* clear any stale leftover */
        fprintf(stderr, "migrate: dry-run, rehearsing on copy %s\n", dry_path);
        if (migrate_snapshot(db_path, dry_path) != 0) {
            fprintf(stderr, "migrate: dry-run copy failed\n");
            printf("status: blocked\n");
            return 1;
        }
        target = dry_path;
    } else if (do_backup) {
        char backup_path[1024];
        snprintf(backup_path, sizeof(backup_path), "%s.backup-pre-migrate-v%d", db_path, current);
        fprintf(stderr, "migrate: snapshotting to %s\n", backup_path);
        if (migrate_snapshot(db_path, backup_path) != 0)
            return 3;
    } else {
        fprintf(stderr, "migrate: --no-backup specified, skipping snapshot\n");
    }

    /* Apply migrations one by one until we reach `latest` or run out. */
    int rc_apply = 0;
    while (current < latest) {
        const migration_t *m = migrations_find(current);
        if (!m) {
            fprintf(stderr,
                    "migrate: no migration registered for user_version=%d; "
                    "this build cannot continue\n",
                    current);
            rc_apply = 2;
            break;
        }

        fprintf(stderr, "migrate: %sapplying v%d -> v%d\n", dry_run ? "dry-run, " : "",
                m->from_version, m->to_version);

        char out[256], err[1024];
        int rc = migrate_exec(minimoni_exec, target, m->script, out, sizeof(out), err, sizeof(err));
        if (rc != 0) {
            fprintf(stderr, "migrate: v%d -> v%d failed (db exec rc=%d)\n", m->from_version,
                    m->to_version, rc);
            if (err[0])
                fputs(err, stderr);
            if (!dry_run && do_backup) {
                fprintf(stderr,
                        "migrate: transaction was rolled back. The snapshot at "
                        "%s.backup-pre-migrate-v%d is intact if you need to "
                        "restore manually.\n",
                        db_path, m->from_version);
            }
            rc_apply = 2;
            break;
        }
        current = m->to_version;
    }

    if (dry_run) {
        remove_db_with_sidecars(dry_path);
        if (rc_apply == 0) {
            fprintf(stderr, "migrate: dry-run OK, no changes written to %s\n", db_path);
            printf("status: migration-pending\n");
            return 0;
        }
        fprintf(stderr, "migrate: dry-run found the migration would fail\n");
        printf("status: blocked\n");
        return 1;
    }

    if (rc_apply != 0)
        return rc_apply;

    /* Close the upgrade with a consolidation + VACUUM: v0.1 stored only raw rows,
     * so the daemon's first consolidation would DELETE the backlog and leave the
     * freed pages bloating the file. Doing it here hands the daemon a compacted DB.
     * Reached only after a real migration, so it runs once, never on a re-run. */
    int rc_compact = migrate_consolidate_and_vacuum(minimoni_exec, db_path);
    if (rc_compact != 0)
        return rc_compact;

    fprintf(stderr, "migrate: success, database at user_version=%d\n", current);
    return 0;
}

int main(int argc, char **argv)
{
    int         do_backup = 1;
    int         dry_run = 0;
    int         force = 0;
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
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
            continue;
        }
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
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

    if (force && !do_backup) {
        fprintf(stderr, "migrate: --force cannot be combined with --no-backup "
                        "(a backup is mandatory when forcing past the fingerprint)\n");
        return 1;
    }

    char        colocated[1024];
    const char *minimoni_exec =
        resolve_minimoni_exec(argv[0], cli_use, colocated, sizeof(colocated));

    return do_migrate(minimoni_exec, db_path, do_backup, dry_run, force);
}
