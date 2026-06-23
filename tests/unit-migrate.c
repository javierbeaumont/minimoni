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

/* Unit tests for the parts of minimoni-migrate that are testable without the
 * fork+exec to `minimoni db exec`:
 *
 *   - migrations.c: the in-binary migration registry (pure data + tiny
 *     accessors).
 *   - snapshot.c:   the pre-migration backup file copy.
 *
 * preflight.c, exec.c and main.c are covered end-to-end by the shell suite
 * tests/migrate.sh because they all depend on the live `minimoni` binary
 * being present at run time (unit-testing them would require injecting the SQL
 * executor as a callback).
 *
 * Standalone (no framework). #includes the migrate .c modules directly so the
 * static helpers are reachable. */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bring in the modules under test. exec.c is pulled in because migrations.c
 * references `migrate_exec` from its verify_v0_schema hook; the tests never
 * invoke that hook, so the fork+exec code path stays dormant. */
#include "../src/migrate/exec.c"
#include "../src/migrate/migrations.c"
#include "../src/migrate/snapshot.c"

/* --- Test infrastructure ------------------------------------------------ */

static int g_counter = 0;

static void temp_path(char *out, size_t out_size, const char *suffix)
{
    snprintf(out, out_size, "/tmp/minimoni-test-migrate-%d-%d-%s", getpid(), g_counter++, suffix);
}

/* Write `n` bytes of `data` to `path`, creating or truncating. Returns 0. */
static int write_file(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 1;
    int rc = (write(fd, data, n) == (ssize_t)n) ? 0 : 1;
    close(fd);
    return rc;
}

/* Read up to `cap` bytes from `path` into `out`. Returns bytes read, -1 on
 * error. Does not NUL-terminate. */
static ssize_t read_file(const char *path, void *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, cap);
    close(fd);
    return n;
}

static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* --- Tests: migrations.c ------------------------------------------------ */

/* The latest migration target is whatever the last entry of the registry
 * lists. As of v0.2 there is exactly one migration (0 -> 1). */
static int test_migrations_latest_version(void)
{
    return (migrations_latest_version() == 1) ? 0 : 1;
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

/* --- Tests: snapshot.c -------------------------------------------------- */

/* Happy path: copy a small file, verify the destination matches the source
 * byte-for-byte and is the expected size. */
static int test_snapshot_copies_file(void)
{
    char src[256], dst[256];
    temp_path(src, sizeof(src), "src.bin");
    temp_path(dst, sizeof(dst), "dst.bin");

    const char payload[] = "minimoni snapshot test payload - 0123456789\n";
    int        ok = (write_file(src, payload, sizeof(payload) - 1) == 0);
    if (ok)
        ok = (migrate_snapshot(src, dst) == 0);
    if (ok) {
        char    buf[256];
        ssize_t n = read_file(dst, buf, sizeof(buf));
        ok = (n == (ssize_t)(sizeof(payload) - 1) && memcmp(buf, payload, n) == 0);
    }

    unlink(src);
    unlink(dst);
    return ok ? 0 : 1;
}

/* Destination already exists with stale contents; snapshot must overwrite
 * (truncate) it rather than refusing. */
static int test_snapshot_overwrites_existing(void)
{
    char src[256], dst[256];
    temp_path(src, sizeof(src), "src2.bin");
    temp_path(dst, sizeof(dst), "dst2.bin");

    const char old[] = "this is stale destination content that should be replaced";
    const char src_bytes[] = "fresh";
    int        ok = (write_file(dst, old, sizeof(old) - 1) == 0 &&
                     write_file(src, src_bytes, sizeof(src_bytes) - 1) == 0);
    if (ok)
        ok = (migrate_snapshot(src, dst) == 0);
    if (ok) {
        long n = file_size(dst);
        ok = (n == (long)(sizeof(src_bytes) - 1));
    }
    if (ok) {
        char    buf[64];
        ssize_t n = read_file(dst, buf, sizeof(buf));
        ok = (n == (ssize_t)(sizeof(src_bytes) - 1) && memcmp(buf, src_bytes, n) == 0);
    }

    unlink(src);
    unlink(dst);
    return ok ? 0 : 1;
}

/* Source path does not exist: snapshot must fail with non-zero return. */
static int test_snapshot_source_missing(void)
{
    char src[256], dst[256];
    temp_path(src, sizeof(src), "missing-src.bin");
    temp_path(dst, sizeof(dst), "dst3.bin");
    unlink(src);
    unlink(dst);

    int rc = migrate_snapshot(src, dst);
    int ok = (rc != 0);
    /* Destination must not have been created on failure. */
    if (ok)
        ok = (file_size(dst) < 0);

    unlink(dst);
    return ok ? 0 : 1;
}

/* Destination path lives in a directory that does not exist: snapshot must
 * fail with non-zero, not crash. */
static int test_snapshot_target_unwritable_dir(void)
{
    char src[256], dst[256];
    temp_path(src, sizeof(src), "src4.bin");
    snprintf(dst, sizeof(dst), "/tmp/minimoni-no-such-dir-%d/dst.bin", getpid());

    const char payload[] = "x";
    if (write_file(src, payload, 1) != 0) {
        unlink(src);
        return 1;
    }

    int rc = migrate_snapshot(src, dst);
    int ok = (rc != 0);

    unlink(src);
    return ok ? 0 : 1;
}

/* Edge case: empty (zero-byte) source. Snapshot must succeed and leave a
 * zero-byte destination. */
static int test_snapshot_zero_byte_source(void)
{
    char src[256], dst[256];
    temp_path(src, sizeof(src), "src5.bin");
    temp_path(dst, sizeof(dst), "dst5.bin");

    if (write_file(src, "", 0) != 0)
        return 1;

    int rc = migrate_snapshot(src, dst);
    int ok = (rc == 0 && file_size(dst) == 0);

    unlink(src);
    unlink(dst);
    return ok ? 0 : 1;
}

/* --- Runner ------------------------------------------------------------- */

#include "runner.h"

static const test_t ALL_TESTS[] = {
    /* migrations.c */
    T(migrations_latest_version),
    T(migrations_find_v0),
    T(migrations_find_unknown),
    T(migrations_v0_script_bumps_user_version),
    /* snapshot.c */
    T(snapshot_copies_file),
    T(snapshot_overwrites_existing),
    T(snapshot_source_missing),
    T(snapshot_target_unwritable_dir),
    T(snapshot_zero_byte_source),
};

int main(void)
{
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
