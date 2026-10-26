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

/* Unit tests for the parts of minimoni-migrate that run without the real `minimoni db exec`, which
 * the seam below replaces: migrations.c, the in-binary migration registry, and snapshot.c's file
 * handling around the copy.
 *
 * Standalone (no framework). #includes the migrate .c modules directly so the static helpers are
 * reachable. */

#define _XOPEN_SOURCE 700

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/migrate/migrations.c"
#include "../src/migrate/snapshot.c"

/* --- The migrate_exec seam --- */

/* The fake `minimoni db exec` records its last call, prints g_exec_out and returns g_exec_rc; on
 * success it writes COPY into the file its value names, as VACUUM INTO would. */
#define COPY "a copy\n"

static int         g_exec_rc;
static const char *g_exec_out;
static char        g_exec_sql[64];
static char        g_exec_value[1200];
static int         g_exec_saw_empty_private; /* that file existed, empty and 0600, when called */

int migrate_exec(const char *minimoni_exec, const char *db_path, const char *sql, const char *value,
                 char *out_buf, size_t out_size, char *err_buf, size_t err_size)
{
    (void)minimoni_exec;
    (void)db_path;

    if (out_size > 0)
        snprintf(out_buf, out_size, "%s", g_exec_out ? g_exec_out : "");
    if (err_size > 0)
        err_buf[0] = '\0';

    snprintf(g_exec_sql, sizeof(g_exec_sql), "%s", sql);
    snprintf(g_exec_value, sizeof(g_exec_value), "%s", value ? value : "");

    struct stat st;
    g_exec_saw_empty_private =
        value && stat(value, &st) == 0 && st.st_size == 0 && (st.st_mode & 0777) == 0600;

    FILE *f = g_exec_rc == 0 && value ? fopen(value, "w") : NULL;
    if (f) {
        fputs(COPY, f);
        fclose(f);
    }

    return g_exec_rc;
}

/* --- Test infrastructure --- */

static char g_dir[64];

/* A fresh directory and a reset seam for each test. */
static void fresh(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/minimoni-unit-migrate-XXXXXX");
    if (!mkdtemp(g_dir))
        g_dir[0] = '\0';

    g_exec_rc = 0;
    g_exec_out = NULL;
    g_exec_sql[0] = '\0';
    g_exec_value[0] = '\0';
    g_exec_saw_empty_private = 0;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;

    return remove(path);
}

static void drop(void) { nftw(g_dir, remove_entry, 8, FTW_DEPTH | FTW_PHYS); }

static void in_dir(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int has_text(const char *path, const char *text)
{
    char  buf[64] = "";
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    return strcmp(buf, text) == 0;
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int private_file(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600;
}

/* --- Tests: migrations.c ------------------------------------------------ */

/* The latest migration target is whatever the last entry of the registry lists. */
static int test_migrations_latest_version(void)
{
    return (migrations_latest_version() == 2) ? 0 : 1;
}

/* The registry contains the 0 -> 1 migration. */
static int test_migrations_find_v0(void)
{
    const migration_t *m = migrations_find(0);
    if (!m)
        return 1;
    if (m->from_version != 0)
        return 1;
    if (m->to_version != 1)
        return 1;
    if (!m->script)
        return 1;
    /* Script must start with BEGIN and end with COMMIT;<eos> for the
     * atomicity contract described in migrations.h. */
    if (strncmp(m->script, "BEGIN;", 6) != 0)
        return 1;
    size_t n = strlen(m->script);
    if (n < 7 || strncmp(m->script + n - 7, "COMMIT;", 7) != 0)
        return 1;
    return 0;
}

/* The 1 -> 2 migration: verified first, atomic, and bumping user_version to 2. */
static int test_migrations_find_v1(void)
{
    const migration_t *m = migrations_find(1);
    if (!m || m->from_version != 1 || m->to_version != 2 || !m->script || !m->verify_source)
        return 1;

    size_t n = strlen(m->script);

    return strncmp(m->script, "BEGIN;", 6) == 0 && n >= 7 &&
                   strncmp(m->script + n - 7, "COMMIT;", 7) == 0 &&
                   strstr(m->script, "PRAGMA user_version = 2")
               ? 0
               : 1;
}

/* Before the 1 -> 2 step, the structure is read, and it must be v0.2's. */
static int test_v1_fingerprint_accepts_a_v02_database(void)
{
    fresh();
    g_exec_out = V1_SCHEMA_FINGERPRINT;

    int rc = migrations_find(1)->verify_source("minimoni", "metrics.db");
    int read_schema = strncmp(g_exec_sql, SCHEMA_QUERY, sizeof(g_exec_sql) - 1) == 0;
    drop();

    return rc == 0 && read_schema ? 0 : 1;
}

static int test_v1_fingerprint_refuses_an_extra_column(void)
{
    char        out[1024];
    const char *end = "uptime_seconds:REAL:0\n";
    fresh();
    snprintf(out, sizeof(out), "%.*suptime_seconds:REAL:0,my_extra_col:TEXT:0\n",
             (int)(strstr(V1_SCHEMA_FINGERPRINT, end) - V1_SCHEMA_FINGERPRINT),
             V1_SCHEMA_FINGERPRINT);
    g_exec_out = out;

    int rc = migrations_find(1)->verify_source("minimoni", "metrics.db");
    drop();

    return rc == 1 ? 0 : 1;
}

static int test_v1_fingerprint_refuses_when_the_schema_cannot_be_read(void)
{
    fresh();
    g_exec_rc = 1;

    int rc = migrations_find(1)->verify_source("minimoni", "metrics.db");
    drop();

    return rc == 1 ? 0 : 1;
}

/* No migration starts from version 99 (unknown future version). */
static int test_migrations_find_unknown(void) { return (migrations_find(99) == NULL) ? 0 : 1; }

/* The v0->v1 script must bump user_version to 1 explicitly, before the final
 * COMMIT, per the migrations.h contract. Test by searching for the literal
 * "PRAGMA user_version = 1" inside the script. */
static int test_migrations_v0_script_bumps_user_version(void)
{
    const migration_t *m = migrations_find(0);
    if (!m || !m->script)
        return 1;
    return (strstr(m->script, "PRAGMA user_version = 1") != NULL) ? 0 : 1;
}

/* --- Tests: snapshot.c --- */

/* VACUUM INTO ?1 writes into a temporary file, already created empty and private, which then takes
 * dst's name. */
static int test_snapshot_copies_through_a_temporary_file(void)
{
    fresh();
    char dst[128], tmp[160];
    in_dir(dst, sizeof(dst), "db.backup");
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);

    int ok = migrate_snapshot("minimoni", "db", dst) == 0 &&
             strcmp(g_exec_sql, "VACUUM INTO ?1") == 0 && strcmp(g_exec_value, tmp) == 0 &&
             g_exec_saw_empty_private && has_text(dst, COPY) && private_file(dst) && !exists(tmp);
    drop();

    return ok ? 0 : 1;
}

static int test_snapshot_replaces_an_earlier_backup(void)
{
    fresh();
    char dst[128];
    in_dir(dst, sizeof(dst), "db.backup");
    write_text(dst, "earlier");
    chmod(dst, 0644);

    int ok =
        migrate_snapshot("minimoni", "db", dst) == 0 && has_text(dst, COPY) && private_file(dst);
    drop();

    return ok ? 0 : 1;
}

static int test_snapshot_is_private_under_any_umask(void)
{
    fresh();
    char dst[128];
    in_dir(dst, sizeof(dst), "db.backup");

    mode_t old = umask(0);
    int    rc = migrate_snapshot("minimoni", "db", dst);
    umask(old);
    int ok = rc == 0 && private_file(dst);
    drop();

    return ok ? 0 : 1;
}

static int test_snapshot_replaces_a_symlink_not_its_target(void)
{
    fresh();
    char dst[128], target[128];
    in_dir(dst, sizeof(dst), "db.backup");
    in_dir(target, sizeof(target), "target");
    write_text(target, "not a backup");
    int ok = symlink(target, dst) == 0;

    ok = ok && migrate_snapshot("minimoni", "db", dst) == 0 && has_text(target, "not a backup") &&
         private_file(dst) && has_text(dst, COPY);
    drop();

    return ok ? 0 : 1;
}

/* Left beside the new copy, an earlier backup's journal would be applied to it. */
static int test_snapshot_removes_an_earlier_backups_leftovers(void)
{
    fresh();
    char dst[128], side[3][160];
    in_dir(dst, sizeof(dst), "db.backup");
    write_text(dst, "earlier");
    const char *suffix[3] = {"-journal", "-wal", "-shm"};
    for (int i = 0; i < 3; i++) {
        snprintf(side[i], sizeof(side[i]), "%s%s", dst, suffix[i]);
        write_text(side[i], "leftover");
    }

    int ok = migrate_snapshot("minimoni", "db", dst) == 0 && has_text(dst, COPY);
    for (int i = 0; i < 3; i++)
        ok = ok && !exists(side[i]);
    drop();

    return ok ? 0 : 1;
}

/* A temporary file that an interrupted earlier run left behind is cleared first. */
static int test_snapshot_clears_a_stale_temporary(void)
{
    fresh();
    char dst[128], tmp[160], tmp_journal[180];
    in_dir(dst, sizeof(dst), "db.backup");
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    snprintf(tmp_journal, sizeof(tmp_journal), "%s-journal", tmp);
    write_text(tmp, "stale");
    write_text(tmp_journal, "stale");

    int ok = migrate_snapshot("minimoni", "db", dst) == 0 && g_exec_saw_empty_private &&
             has_text(dst, COPY) && !exists(tmp) && !exists(tmp_journal);
    drop();

    return ok ? 0 : 1;
}

static int test_snapshot_failed_copy_keeps_the_earlier_backup(void)
{
    fresh();
    char dst[128], tmp[160];
    in_dir(dst, sizeof(dst), "db.backup");
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    write_text(dst, "earlier");
    g_exec_rc = 1;

    int ok =
        migrate_snapshot("minimoni", "db", dst) != 0 && has_text(dst, "earlier") && !exists(tmp);
    drop();

    return ok ? 0 : 1;
}

static int test_snapshot_onto_a_directory_fails(void)
{
    fresh();
    char dst[128], inside[160], tmp[160];
    in_dir(dst, sizeof(dst), "db.backup");
    snprintf(inside, sizeof(inside), "%s/inside", dst);
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    mkdir(dst, 0700);
    mkdir(inside, 0700);

    int ok = migrate_snapshot("minimoni", "db", dst) != 0 && exists(inside) && !exists(tmp);
    drop();

    return ok ? 0 : 1;
}

/* Refused before anything is created or run. */
static int test_snapshot_path_too_long_fails(void)
{
    fresh();
    char dst[1200];
    snprintf(dst, sizeof(dst), "%s/", g_dir);
    size_t len = strlen(dst);
    memset(dst + len, 'a', sizeof(dst) - 1 - len);
    dst[sizeof(dst) - 1] = '\0';

    int ok = migrate_snapshot("minimoni", "db", dst) != 0 && g_exec_sql[0] == '\0';
    drop();

    return ok ? 0 : 1;
}

/* --- Runner ------------------------------------------------------------- */

#include "runner.h"

static const test_t ALL_TESTS[] = {
    /* migrations.c */
    T(migrations_latest_version),
    T(migrations_find_v0),
    T(migrations_find_v1),
    T(v1_fingerprint_accepts_a_v02_database),
    T(v1_fingerprint_refuses_an_extra_column),
    T(v1_fingerprint_refuses_when_the_schema_cannot_be_read),
    T(migrations_find_unknown),
    T(migrations_v0_script_bumps_user_version),
    /* snapshot.c */
    T(snapshot_copies_through_a_temporary_file),
    T(snapshot_replaces_an_earlier_backup),
    T(snapshot_is_private_under_any_umask),
    T(snapshot_replaces_a_symlink_not_its_target),
    T(snapshot_removes_an_earlier_backups_leftovers),
    T(snapshot_clears_a_stale_temporary),
    T(snapshot_failed_copy_keeps_the_earlier_backup),
    T(snapshot_onto_a_directory_fails),
    T(snapshot_path_too_long_fails),
};

int main(void)
{
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
