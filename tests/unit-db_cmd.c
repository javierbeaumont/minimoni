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

/* Unit tests for src/db_cmd.c: the pure human-readable formatters and the
 * db_cmd_info read-only inspector. Zero-dependency, no framework; build with
 * `make test`. db_cmd.c is #included directly so the static helpers are
 * exercisable, and the suite links vendor/sqlite3.c (db_cmd_info and the test
 * fixtures both use the SQLite API). */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "runner.h"

#include "../src/db_cmd.c"

/* --- pure formatters --- */

static int eq(const char *got, const char *want) { return strcmp(got, want) == 0; }

static int chk_appid(long app_id, long version, const char *want)
{
    char b[64];
    format_application_id(app_id, version, b, sizeof(b));
    return eq(b, want);
}

static int chk_bucket(long bucket_sec, const char *want)
{
    char b[16];
    format_bucket_label(bucket_sec, b, sizeof(b));
    return eq(b, want);
}

static int chk_dur(double seconds, const char *want)
{
    char b[32];
    format_duration(seconds, b, sizeof(b));
    return eq(b, want);
}

static int chk_size(long bytes, const char *want)
{
    char b[32];
    format_size(bytes, b, sizeof(b));
    return eq(b, want);
}

static int test_format_application_id(void)
{
    /* moni magic -> "moni vN"; anything else -> hex (no version), never decoded as text */
    return (chk_appid(0x6D6F6E69L, 1, "moni v1") &&
            chk_appid(0x12345678L, 2, "0x12345678 (not a minimoni database)") &&
            chk_appid(0, 0, "0x0 (not a minimoni database)"))
               ? 0
               : 1;
}

static int test_format_bucket_label(void)
{
    return (chk_bucket(1, "1s") && chk_bucket(5, "5s") && chk_bucket(30, "30s") &&
            chk_bucket(60, "1m") && chk_bucket(300, "5m") && chk_bucket(3600, "1h") &&
            chk_bucket(21600, "6h") && chk_bucket(86400, "1d"))
               ? 0
               : 1;
}

static int test_format_duration(void)
{
    return (chk_dur(-5.0, "0 seconds") && chk_dur(45.0, "45 seconds") &&
            chk_dur(600.0, "10 minutes") && chk_dur(7200.0, "2.0 hours") &&
            chk_dur(172800.0, "2.0 days") && chk_dur(86400.0 * 365.25 * 3.0, "3.00 years"))
               ? 0
               : 1;
}

static int test_format_size(void)
{
    return (chk_size(0, "0 bytes") && chk_size(512, "512 bytes") && chk_size(1536, "1.5 KB") &&
            chk_size(2621440, "2.5 MB") && chk_size(3L * 1024 * 1024 * 1024, "3.0 GB"))
               ? 0
               : 1;
}

static int test_format_timestamp_utc(void)
{
    /* Pin the zone so the test is deterministic regardless of the host. UTC0 is
     * a POSIX TZ string musl resolves without tzdata. */
    setenv("TZ", "UTC0", 1);
    tzset();
    char b[48];
    format_timestamp((time_t)0, b, sizeof(b));
    if (!eq(b, "1970-01-01 00:00:00 UTC"))
        return 1;
    format_timestamp((time_t)1000000000, b, sizeof(b));
    if (!eq(b, "2001-09-09 01:46:40 UTC"))
        return 1;
    return 0;
}

/* --- db_cmd_info: fixtures + behaviour --- */

static int  g_dbcmd_ctr = 0;
static char g_dbcmd_path[256];

/* Build a DB with a metrics table at the given header. with_bucket adds the
 * bucket_sec column; n inserts that many rows (distinct T+Z timestamps,
 * bucket_sec = 60 when present). Returns 0 on success, -1 on error. */
static int build_db(const char *path, long app_id, long user_version, int with_bucket, int n)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "CREATE TABLE metrics (timestamp TEXT, load_1m REAL%s);"
             "PRAGMA application_id = %ld; PRAGMA user_version = %ld;",
             with_bucket ? ", bucket_sec INTEGER" : "", app_id, user_version);
    int rc = sqlite3_exec(db, hdr, NULL, NULL, NULL);
    for (int i = 0; rc == SQLITE_OK && i < n; i++) {
        char ins[256];
        snprintf(ins, sizeof(ins),
                 "INSERT INTO metrics (timestamp, load_1m%s)"
                 " VALUES ('2001-09-09T01:46:%02dZ', 1.0%s)",
                 with_bucket ? ", bucket_sec" : "", i, with_bucket ? ", 60" : "");
        rc = sqlite3_exec(db, ins, NULL, NULL, NULL);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Build a valid minimoni DB (moni magic) with NO metrics table. */
static int build_empty_db(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    char sql[256];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE other (x);"
             "PRAGMA application_id = %ld; PRAGMA user_version = 1;",
             0x6D6F6E69L);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/* A fresh, not-yet-existing temp DB path. */
static const char *dbcmd_tmp(void)
{
    snprintf(g_dbcmd_path, sizeof(g_dbcmd_path), "/tmp/minimoni-dbcmd-%d-%d.db", getpid(),
             g_dbcmd_ctr++);
    unlink(g_dbcmd_path);
    return g_dbcmd_path;
}

/* Run db_cmd_info(db_path) with stdout redirected to a temp file; copy up to
 * cap-1 bytes of the captured output into out and return db_cmd_info's rc. */
static int run_info_capture(const char *db_path, char *out, size_t cap)
{
    char cap_path[256];
    snprintf(cap_path, sizeof(cap_path), "/tmp/minimoni-dbcmd-cap-%d.txt", getpid());

    fflush(stdout);
    int   saved = dup(STDOUT_FILENO);
    FILE *cf = fopen(cap_path, "w+");
    if (saved < 0 || !cf) {
        if (cf)
            fclose(cf);
        return -99;
    }
    dup2(fileno(cf), STDOUT_FILENO);
    int rc = db_cmd_info(db_path);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    rewind(cf);
    size_t got = fread(out, 1, cap - 1, cf);
    out[got] = '\0';
    fclose(cf);
    unlink(cap_path);
    return rc;
}

static int test_dbinfo_foreign(void)
{
    const char *p = dbcmd_tmp();
    if (build_db(p, 0x12345678L, 1, 1, 2) != 0)
        return 1;
    char out[2048];
    int  rc = run_info_capture(p, out, sizeof(out));
    unlink(p);
    /* foreign application_id: render as hex (never decoded as text) and stop;
     * a non-minimoni file's tables are not read as minimoni metrics. */
    return (rc == 0 && strstr(out, "(not a minimoni database)") && !strstr(out, "moni v") &&
            !strstr(out, "Total rows"))
               ? 0
               : 1;
}

static int test_dbinfo_minimoni(void)
{
    const char *p = dbcmd_tmp();
    if (build_db(p, 0x6D6F6E69L, 1, 1, 3) != 0)
        return 1;
    char out[2048];
    int  rc = run_info_capture(p, out, sizeof(out));
    unlink(p);
    return (rc == 0 && strstr(out, "moni v1") && strstr(out, "Total rows") && strstr(out, "1m"))
               ? 0
               : 1;
}

static int test_dbinfo_missing_file(void)
{
    /* dbcmd_tmp() returns an unlinked path; stat fails -> error exit. */
    return db_cmd_info(dbcmd_tmp()) == 1 ? 0 : 1;
}

static int test_dbinfo_no_metrics(void)
{
    const char *p = dbcmd_tmp();
    if (build_empty_db(p) != 0)
        return 1;
    char out[2048];
    int  rc = run_info_capture(p, out, sizeof(out));
    unlink(p);
    return (rc == 0 && strstr(out, "no metrics table")) ? 0 : 1;
}

static int test_dbinfo_v01(void)
{
    /* minimoni v0.1 database: moni magic, user_version 0, no bucket_sec. The
     * version comes from user_version and there are no tiers to show. */
    const char *p = dbcmd_tmp();
    if (build_db(p, 0x6D6F6E69L, 0, 0, 2) != 0) /* v0.1: user_version 0, no bucket_sec */
        return 1;
    char out[2048];
    int  rc = run_info_capture(p, out, sizeof(out));
    unlink(p);
    return (rc == 0 && strstr(out, "moni v0") && strstr(out, "no tiered data") &&
            strstr(out, "Total rows"))
               ? 0
               : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    /* pure formatters */
    T(format_application_id),
    T(format_bucket_label),
    T(format_duration),
    T(format_size),
    T(format_timestamp_utc),
    /* db_cmd_info behaviour */
    T(dbinfo_foreign),
    T(dbinfo_minimoni),
    T(dbinfo_missing_file),
    T(dbinfo_no_metrics),
    T(dbinfo_v01),
};

int main(void)
{
    freopen("/dev/null", "w", stderr); /* silence db_cmd_info's intentional error lines */
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
